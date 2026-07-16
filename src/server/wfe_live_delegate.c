/* wfe_live_delegate.c: the live delegate provider for the workflow engine.
 *
 * Phase B of full-autonomous-development. Bridges the wfe_delegate_provider seam
 * (Phase A) to aimee's real delegate execution: the autonomous primary MANAGES
 * (the engine + this bridge) while delegates DO the work. For a producing block
 * (author/implement/document) this routes the block's role to a configured agent,
 * runs it WITH TOOLS pinned to the work-item worktree so it edits files in place,
 * then stages + commits the result on the work-item branch.
 *
 * Verification (build/test/lint, reviewer panel, adversarial refute) and the
 * re-delegate-on-reject loop are the engine's gate blocks (gate.ci /
 * gate.roundtable with on_fail looping back to implement) — see build.yaml — so
 * this bridge stays the narrow "run one delegate, commit its work" primitive and
 * the manager loop lives in the (auditable) engine composition.
 *
 * Registered DEFAULT-ON at server_init: autonomous development is core
 * functionality. Registration alone runs nothing — a run only begins when intake
 * creates a work item and the autonomy driver advances it. */
#include "aimee.h"

#include "wfe_live_delegate.h"

#include "agent_config.h"
#include "agent_exec.h"
#include "agent_tools.h"
#include "agent_types.h"
#include "cJSON.h"
#include "coord_jobs.h"
#include "delegate_role.h"
#include "persona.h"
#include "provider_catalog.h"
#include "headers/git_verify.h"
#include "log.h"
#include "util.h"
#include "wfe_approval.h"
#include "wfe_blocks.h"
#include "wfe_live_forge.h"
#include "wfe_live_foreach.h"
#include "wfe_live_panel.h"
#include "wfe_roundtable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Block (busy-poll) until a single-task coord job reaches a terminal state, then
 * hand back the delegate's result text. WFE runs on the autonomy scheduler
 * thread; the coord DISPATCHER runs the task on its own thread through the shared
 * delegate path, so this only waits — it never executes the delegate. Returns 0
 * and fills result_out on 'done'; -1 (+err) on 'failed'/'cancelled'/timeout. */
static int wfe_coord_task_wait(int job_id, int task_id, char *result_out, size_t result_cap,
                               char *err, size_t errlen)
{
   (void)task_id;              /* the job holds exactly one task */
   const int max_polls = 1600; /* 1600 * 750ms ~= 20 min, matching the delegate timeout ceiling */
   for (int i = 0; i < max_polls; i++)
   {
      db1_coord_task_t task;
      memset(&task, 0, sizeof task);
      if (db1_coord_job_list_tasks(job_id, &task, 1) >= 1)
      {
         if (strcmp(task.status, "done") == 0)
         {
            if (result_out && result_cap)
               snprintf(result_out, result_cap, "%s", task.result);
            return 0;
         }
         if (strcmp(task.status, "failed") == 0 || strcmp(task.status, "cancelled") == 0)
         {
            if (err && errlen)
               snprintf(err, errlen, "wfe delegate task %s: %s", task.status,
                        task.error[0] ? task.error : "no detail");
            return -1;
         }
      }
      struct timespec ts = {0, 750L * 1000L * 1000L};
      nanosleep(&ts, NULL);
   }
   if (err && errlen)
      snprintf(err, errlen, "wfe delegate task timed out");
   return -1;
}

/* The live delegate run. Contract per wfe_delegate_provider_t.
 *
 * WFE ORCHESTRATES delegates; it does not run them. This enqueues ONE delegate
 * task onto the coord queue — aimee's single delegate-dispatch queue — and waits
 * for the coord dispatcher to run it through the shared delegate path. That path
 * owns everything WFE must not: agent routing, the per-model/provider concurrency
 * limit (registered max_parallel), credential leasing/retry, the parent-write
 * guard, and the isolated worktree whose diff it applies back into `workdir`
 * (container-ready — WFE never assumes a local process or a shared checkout).
 * WFE holds no server_ctx, spawns no delegate, and runs no git: `freeze` owns the
 * commit, so out_commit_sha is left empty here. The block's `role` arg is the
 * delegate PERSONA (architect/engineer/...); a producing delegate routes on a
 * WRITE role so the delegate system isolates + applies its changes. */
static int wfe_live_delegate_run(const char *workdir, const char *role, const char *delegate,
                                 const char *prompt, const char *artifact_path,
                                 char out_commit_sha[64], char *err, size_t errlen)
{
   (void)delegate; /* agent selection is the delegate system's routing decision */
   if (out_commit_sha)
      out_commit_sha[0] = '\0';
   if (!workdir || !workdir[0] || !prompt || !prompt[0])
   {
      if (err && errlen)
         snprintf(err, errlen, "wfe live delegate: missing workdir/prompt");
      return -1;
   }

   const char *persona = (role && role[0]) ? role : "engineer";
   int job_id = db1_coord_job_create(WFE_COORD_PLAN_ID, 1);
   if (job_id <= 0)
   {
      if (err && errlen)
         snprintf(err, errlen, "wfe live delegate: could not create coord job");
      return -1;
   }
   /* Write role ("code") so the shared path runs the delegate in an isolated
    * worktree and applies its diff back into `workdir`. cwd = the work-item
    * worktree; persona names the delegate identity. */
   int task_id = db1_coord_job_add_task(job_id, 0, "[]", "code", prompt, workdir, persona);
   if (task_id <= 0)
   {
      db1_coord_job_cancel(job_id);
      if (err && errlen)
         snprintf(err, errlen, "wfe live delegate: could not enqueue coord task");
      return -1;
   }

   char result[DB1_COORD_RESULT_LEN] = "";
   if (wfe_coord_task_wait(job_id, task_id, result, sizeof result, err, errlen) != 0)
      return -1;

   /* Text-artifact blocks (author.proposal/plan): persist the delegate's reply as
    * the artifact if it did not itself write the file, so the next gate has
    * content. Pure orchestration glue — not delegate execution. */
   if (artifact_path && artifact_path[0] && result[0])
   {
      long existing = -1;
      FILE *rf = fopen(artifact_path, "rb");
      if (rf)
      {
         fseek(rf, 0, SEEK_END);
         existing = ftell(rf);
         fclose(rf);
      }
      if (existing <= 0)
      {
         FILE *wf = fopen(artifact_path, "wb");
         if (wf)
         {
            fwrite(result, 1, strlen(result), wf);
            fclose(wf);
         }
      }
   }
   return 0;
}

static const wfe_delegate_provider_t WFE_LIVE_DELEGATE = {wfe_live_delegate_run};

/* Live verify provider (WP-1b): run the mechanical verify gate synchronously on
 * `workdir` and return the structured format=json verdict. The implement block
 * gates a unit's advance on verdict:passed. Returns 0 + fills out_verdict, or -1
 * if no verdict text came back (the block treats that as a non-pass, fail closed). */
static int wfe_live_verify_run(const char *workdir, char *out_verdict, size_t n)
{
   if (out_verdict && n)
      out_verdict[0] = '\0';
   cJSON *args = cJSON_CreateObject();
   if (!args)
      return -1;
   cJSON_AddStringToObject(args, "action", "run");
   cJSON_AddBoolToObject(args, "async", 0); /* synchronous: we need the verdict now */
   cJSON_AddStringToObject(args, "format", "json");
   /* handle_git_verify never reads a "path" arg itself — on the MCP route the
    * dispatch layer chdirs the run_cmd thread before the handler runs, and
    * resolve_verify_root() picks the root up from that CWD. Calling the handler
    * directly (as we do here) skips that layer, so the verdict silently came
    * from the DAEMON's CWD, not the work-item worktree: the steps ran against
    * a non-repo dir and vacuously passed, verifying nothing. Pin the run_cmd
    * thread CWD to the worktree for the duration, exactly like the delegate
    * run above. */
   if (workdir && workdir[0])
      run_cmd_set_cwd(workdir);
   cJSON *resp = handle_git_verify(NULL, args, NULL);
   run_cmd_set_cwd(NULL);
   cJSON_Delete(args);
   int rc = -1;
   if (resp)
   {
      const cJSON *item = cJSON_GetArrayItem(resp, 0);
      const cJSON *text = item ? cJSON_GetObjectItemCaseSensitive(item, "text") : NULL;
      if (cJSON_IsString(text) && text->valuestring)
      {
         snprintf(out_verdict, n, "%s", text->valuestring);
         rc = 0;
      }
      cJSON_Delete(resp);
   }
   return rc;
}

static const wfe_verify_provider_t WFE_LIVE_VERIFY = {wfe_live_verify_run};

/* Live judge provider (PC3): dispatch a reviewer/skeptic delegate to READ-ONLY judge
 * the change in `workdir` and emit a {"refuted":bool} verdict. The delegate runs with
 * tools (it can `git diff` + read files) but we do NOT commit — this is a judgment,
 * not a producing step. Fail-closed: any dispatch error / a response that does not
 * carry an explicit `"refuted": false` is treated as REFUTED, so the adversarial gate
 * can never advance on an ambiguous verdict. */
static int wfe_live_judge_run(const char *workdir, const char *lens, char *out_verdict, size_t n)
{
   if (out_verdict && n)
      out_verdict[0] = '\0';
   if (!workdir || !workdir[0])
      return -1;
   int is_skeptic = (lens && strcmp(lens, "skeptic") == 0);
   /* reviewer -> the reviewer persona; skeptic -> the (adversarial) security persona,
    * with the REFUTE framing carried by the prompt (lens diversity). */
   const char *persona = is_skeptic ? "security" : "reviewer";

   char prompt[2048];
   if (is_skeptic)
      snprintf(
          prompt, sizeof prompt,
          "You are an ADVERSARIAL skeptic reviewing an autonomous code change. Inspect the "
          "change in this git worktree (e.g. `git diff HEAD~5..HEAD` and read the touched "
          "files). Try HARD to REFUTE it: find a correctness bug, a broken or missing test, an "
          "unmet requirement, or a security regression. Do NOT edit files. End your reply with "
          "EXACTLY one JSON line and nothing after it: {\"refuted\": true, \"reason\": \"<the "
          "flaw>\"} if you found a real flaw, or {\"refuted\": false} only if you genuinely "
          "cannot refute it.");
   else
      snprintf(
          prompt, sizeof prompt,
          "Review the autonomous code change in this git worktree (e.g. `git diff "
          "HEAD~5..HEAD` and read the touched files) for correctness and quality. Do NOT edit "
          "files. End your reply with EXACTLY one JSON line and nothing after it: {\"refuted\": "
          "false} if the change is sound, or {\"refuted\": true, \"reason\": \"<why>\"} if it "
          "is flawed.");

   /* Enqueue a READ-ONLY delegate task on the coord queue. role "review" is not a
    * write role, so the shared path's write-capable gate blocks any file edit at
    * the tool layer — the read-only property is enforced by the delegate system,
    * not by a WFE-side hard-reset. WFE just reads the verdict. */
   int job_id = db1_coord_job_create(WFE_COORD_PLAN_ID, 1);
   if (job_id <= 0)
   {
      snprintf(out_verdict, n, "{\"refuted\":true,\"reason\":\"could not create coord job\"}");
      return 0; /* fail-closed verdict */
   }
   int task_id = db1_coord_job_add_task(job_id, 0, "[]", "review", prompt, workdir, persona);
   if (task_id <= 0)
   {
      db1_coord_job_cancel(job_id);
      snprintf(out_verdict, n, "{\"refuted\":true,\"reason\":\"could not enqueue coord task\"}");
      return 0;
   }

   char result[DB1_COORD_RESULT_LEN] = "";
   int ok = wfe_coord_task_wait(job_id, task_id, result, sizeof result, NULL, 0);

   int refuted = 1; /* fail-closed default */
   if (ok == 0 && result[0])
   {
      /* Parse ONLY the LAST non-empty line as the verdict JSON (the delegate is told
       * to emit exactly one JSON line at the end). A substring scan of the whole
       * response would false-ACCEPT on a skeptic's reasoning that quotes
       * {"refuted": false}. Accept only on an explicit boolean refuted:false; anything
       * that doesn't parse to that is REFUTED (fail closed). */
      const char *r = result;
      size_t len = strlen(r);
      while (len > 0 &&
             (r[len - 1] == '\n' || r[len - 1] == '\r' || r[len - 1] == ' ' || r[len - 1] == '\t'))
         len--;
      size_t start = len;
      while (start > 0 && r[start - 1] != '\n')
         start--;
      size_t llen = len - start;
      char line[512];
      if (llen > 0 && llen < sizeof line)
      {
         memcpy(line, r + start, llen);
         line[llen] = '\0';
         cJSON *doc = cJSON_Parse(line);
         if (doc)
         {
            const cJSON *rf = cJSON_GetObjectItemCaseSensitive(doc, "refuted");
            if (rf && cJSON_IsFalse(rf))
               refuted = 0;
            cJSON_Delete(doc);
         }
      }
   }
   snprintf(out_verdict, n, "{\"refuted\":%s}", refuted ? "true" : "false");
   return 0; /* a verdict was produced (even a fail-closed one) */
}

static const wfe_judge_provider_t WFE_LIVE_JUDGE = {wfe_live_judge_run};

/* --- live mechanical-verify provider (the implement gate) ---------------------
 * Runs the git_verify gate synchronously in the work-item worktree and hands
 * its format=json verdict document to wfe_implement_verify_ok. Without a
 * registered provider the gate fails closed, which meant implement could NEVER
 * advance on a real deployment (the provider only ever existed in tests —
 * observed live as instant impl loops to the cap on every slice child). */
static int live_verify_run(const char *workdir, char *out, size_t n)
{
   if (!out || n == 0)
      return -1;
   out[0] = '\0';
   cJSON *args = cJSON_CreateObject();
   if (!args)
      return -1;
   cJSON_AddStringToObject(args, "action", "run");
   cJSON_AddStringToObject(args, "format", "json");
   cJSON_AddBoolToObject(args, "async", 0);
   run_cmd_set_cwd(workdir);
   cJSON *res = handle_git_verify(server_active_ctx(), args, NULL);
   run_cmd_set_cwd(NULL);
   cJSON_Delete(args);
   if (!res)
      return -1;
   /* mcp_text shape: [ {type:"text", text:"<verdict json>"} ] */
   const cJSON *item = cJSON_GetArrayItem(res, 0);
   const cJSON *txt = item ? cJSON_GetObjectItemCaseSensitive(item, "text") : NULL;
   int rc = -1;
   if (txt && cJSON_IsString(txt) && txt->valuestring)
   {
      snprintf(out, n, "%s", txt->valuestring);
      rc = 0;
   }
   cJSON_Delete(res);
   return rc;
}

static const wfe_verify_provider_t LIVE_VERIFY = {live_verify_run};

void wfe_autonomy_register(void)
{
   /* Full engine executor set so a work item can run end-to-end server-side. */
   wfe_register_default_executors();
   wfe_register_roundtable_gate();
   /* The live roundtable panel (per-persona review delegates -> verdicts). Replaces the
    * default fail-closed stub so gate.roundtable can actually convene; still fail-closed
    * (DEGRADED/park) when a required persona has no reachable agent. */
   wfe_live_panel_register();
   wfe_register_human_gate();
   /* The live worker + the mechanical verify gate (implement only advances a unit
    * that passes verification). */
   wfe_set_delegate_provider(&WFE_LIVE_DELEGATE);
   wfe_set_verify_provider(&WFE_LIVE_VERIFY);
   /* The live adversarial judge (PC3): reviewer + skeptic verdicts for the implement
    * adversarial gate. Inert unless AIMEE_AUTONOMY_SKEPTICS>0 (default off), so
    * registration alone changes nothing. */
   wfe_set_judge_provider(&WFE_LIVE_JUDGE);
   /* The live forge (F4): registers a real push+PR+merge provider ONLY if the
    * operator enabled wfe_live_forge_enabled (default OFF). While off, the engine
    * keeps its fail-closed forge stub, so pr.open re-loops and merge parks. */
   wfe_live_forge_register();
   /* The live foreach.workflow spawner: fans a parent run's split packets out to
    * child "slice" runs. Registration alone changes nothing (a run only reaches the
    * foreach node after its plan is authored + roundtabled); without it the foreach
    * node fails closed (parks). */
   wfe_live_foreach_register();
   wfe_set_verify_provider(&LIVE_VERIFY);
   aimee_log(LOG_INFO, "wfe", "autonomous development registered (default-on)");
}
