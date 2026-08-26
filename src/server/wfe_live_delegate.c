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
#include "modules/git/git_forge_vault.h"

#include "wfe_live_delegate.h"

#include "agent_config.h"
#include "agent_exec.h"
#include <aimee/tools/agent_tools.h>
#include "agent_types.h"
#include "cJSON.h"
#include "coord_jobs.h"
#include <aimee/delegates/delegate_backend.h> /* run verify steps inside the delegate sandbox */
#include <aimee/delegates/delegate_role.h>
#include <aimee/delegates/delegate_sandbox_image.h> /* resolve the work item's sandbox image */
#include "sandbox_learned.h" /* learn verify's apt installs -> pre-bake next image */
#include "persona.h"
#include "provider_catalog.h"
#include "modules/git/git_verify.h"
#include "log.h"
#include "util.h"
#include "wfe_approval.h"
#include "wfe_blocks.h"
#include "wfe_live_forge.h"
#include "wfe_live_foreach.h"
#if AIMEE_WITH_ROUNDTABLE
#include "wfe_live_panel.h"
#include "wfe_roundtable.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Block (busy-poll) until a single-task coord job reaches a terminal state, then
 * hand back the delegate's result text. WFE runs on the autonomy scheduler
 * thread; the coord DISPATCHER runs the task on its own thread through the shared
 * delegate path, so this only waits — it never executes the delegate. Returns
 * OK and fills result_out on 'done', NO_CHANGE for the write-role detector's
 * stable no-op outcome, or ERROR (+err) on other failures/cancellation/timeout. */
static wfe_delegate_result_t wfe_coord_task_wait(int job_id, int task_id, char *result_out,
                                                 size_t result_cap, char *err, size_t errlen)
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
            return WFE_DELEGATE_OK;
         }
         if (strcmp(task.status, "failed") == 0 || strcmp(task.status, "cancelled") == 0)
         {
            if (strcmp(task.status, "failed") == 0 && wfe_delegate_error_is_no_change(task.error))
               return WFE_DELEGATE_NO_CHANGE;
            if (err && errlen)
               snprintf(err, errlen, "wfe delegate task %s: %s", task.status,
                        task.error[0] ? task.error : "no detail");
            return WFE_DELEGATE_ERROR;
         }
      }
      struct timespec ts = {0, 750L * 1000L * 1000L};
      nanosleep(&ts, NULL);
   }
   if (err && errlen)
      snprintf(err, errlen, "wfe delegate task timed out");
   return WFE_DELEGATE_ERROR;
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
 * delegate PERSONA (architect/engineer/...); the delegate ROLE (read vs write) is
 * derived from whether the block named a captured artifact_path — see below. */
static void wfe_commit_worktree_changes(const char *workdir);

static int wfe_live_delegate_run(const char *workdir, const char *role, const char *prompt,
                                 const char *artifact_path, char out_commit_sha[64], char *err,
                                 size_t errlen)
{
   if (out_commit_sha)
      out_commit_sha[0] = '\0';
   if (!workdir || !workdir[0] || !prompt || !prompt[0])
   {
      if (err && errlen)
         snprintf(err, errlen, "wfe live delegate: missing workdir/prompt");
      return -1;
   }

   const char *persona = (role && role[0]) ? role : "engineer";
   /* Produce-via-reply vs worktree-mutating, keyed on whether the block named a
    * captured artifact_path:
    *   - artifact_path set (author/understand/split/review): the delegate's OUTPUT
    *     is its reply, which WFE persists below. A READ role ("review"): it
    *     modifies no files, so the shared path's named-file drift guard never
    *     hard-fails it on a repo-relative path quoted in the reference content
    *     threaded into its prompt.
    *   - artifact_path NULL (implement/decompose/tdd/document): the delegate
    *     MUTATES the worktree. A WRITE role ("code") so the delegate system
    *     isolates its checkout and applies the diff back into `workdir`.
    * cwd = the work-item worktree; persona names the delegate identity. */
   int produce_via_reply = (artifact_path && artifact_path[0]);
   const char *delegate_role = produce_via_reply ? "review" : "code";
   int job_id = db1_coord_job_create(WFE_COORD_PLAN_ID, 1);
   if (job_id <= 0)
   {
      if (err && errlen)
         snprintf(err, errlen, "wfe live delegate: could not create coord job");
      return -1;
   }
   int task_id = db1_coord_job_add_task(job_id, 0, "[]", delegate_role, prompt, workdir, persona);
   if (task_id <= 0)
   {
      db1_coord_job_cancel(job_id);
      if (err && errlen)
         snprintf(err, errlen, "wfe live delegate: could not enqueue coord task");
      return -1;
   }

   char result[DB1_COORD_RESULT_LEN] = "";
   wfe_delegate_result_t task_result =
       wfe_coord_task_wait(job_id, task_id, result, sizeof result, err, errlen);
   if (task_result != WFE_DELEGATE_OK)
   {
      /* A timed-out/failed coord wait must not leave an admitted delegate
       * running after the workflow has moved on. Cancellation is idempotent for
       * an already-terminal job, and the agent loop observes it cooperatively. */
      if (task_result == WFE_DELEGATE_ERROR)
         db1_coord_job_cancel(job_id);
      return task_result;
   }

   /* A worktree-mutating (implement/decompose/tdd/document) delegate edits the
    * dedicated per-slice worktree in place; stage + commit its work here so the
    * verify gate records a diff and the PR carries it. Produce-via-reply (read)
    * blocks touch no files, so this is skipped for them. */
   if (!produce_via_reply)
      wfe_commit_worktree_changes(workdir);

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

/* Stage and commit whatever an implement-style delegate wrote directly into the
 * work-item worktree, on its current (work-item) branch. A WFE per-slice tree is
 * DEDICATED: the delegate edits `workdir` in place, so unlike a sibling-worktree
 * delegate there is no isolated checkout whose diff the delegate system applies
 * back (and commits) for us — the change would otherwise sit untracked, the
 * verify gate would see no recorded diff, the implement block would loop to its
 * cap, and the eventual PR would carry nothing. Commit runs server-side (host):
 * the repo object store is writable here even though the delegate's container
 * mounts it read-only. A true no-op (clean tree, or a delegate that committed
 * itself) stages nothing and this is skipped. The author is a fixed WFE identity
 * with no AI attribution, per repo policy. */
static void wfe_commit_worktree_changes(const char *workdir)
{
   if (!workdir || !workdir[0])
      return;
   char cmd[MAX_PATH_LEN + 256];
   int rc = 0;
   snprintf(cmd, sizeof cmd, "git -C '%s' add -A 2>&1", workdir);
   free(run_cmd(cmd, &rc));
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "wfe-delegate", "implement commit: git add failed in %s", workdir);
      return;
   }
   /* `git diff --cached --quiet` exits 0 when nothing is staged, 1 when there is
    * a staged change to commit. Skip the commit (and its noisy "nothing to
    * commit" failure) on a clean tree. */
   snprintf(cmd, sizeof cmd, "git -C '%s' diff --cached --quiet", workdir);
   free(run_cmd(cmd, &rc));
   if (rc == 0)
      return; /* nothing staged: a genuine no-op or an already-committing delegate */
   /* Commit as the installed operator identity, never a wfe persona. The identity
    * is sealed at installation (git/author_name + git/author_email); a deployment
    * without it would produce a commit with no author, which git refuses, so this
    * refuses first and says what to configure. The old aimee-wfe author also made
    * GitHub attach a Co-authored-by trailer to the squash of every PR carrying
    * both authors, which the standing directive forbids. */
   char au_name[256] = "", au_email[256] = "";
   if (git_identity_resolve(workdir, au_name, sizeof au_name, au_email, sizeof au_email) != 1)
   {
      aimee_log(LOG_WARN, "wfe-delegate",
                "no git identity configured; refusing to commit in %s (set user.name/user.email on "
                "the checkout, or seal AIMEE_GIT_AUTHOR_NAME/_EMAIL at install)",
                workdir);
      return;
   }
   char *qn = shell_quote(au_name);
   char *qe = shell_quote(au_email);
   snprintf(cmd, sizeof cmd,
            "git -C '%s' -c user.name=%s -c user.email=%s commit -q "
            "-m 'wfe: apply implement changes' 2>&1",
            workdir, qn ? qn : "", qe ? qe : "");
   free(qn);
   free(qe);
   char *out = run_cmd(cmd, &rc);
   if (rc != 0)
      aimee_log(LOG_WARN, "wfe-delegate", "implement commit failed in %s: %s", workdir,
                out ? out : "");
   else
      aimee_log(LOG_INFO, "wfe-delegate", "committed implement changes in %s", workdir);
   free(out);
}

static const wfe_delegate_provider_t WFE_LIVE_DELEGATE = {wfe_live_delegate_run};

/* Mechanical-verify default step timeout — mirrors git_verify_step.c's default so
 * a step run in the sandbox gets the same wall budget as in-process. */
#define WFE_VERIFY_STEP_TIMEOUT_MS (30 * 60 * 1000)

/* Run the resolved verify steps INSIDE the work item's delegate sandbox — the
 * `--network none`, toolchain-baked image the engineer delegate builds against —
 * instead of in the aimee-server process (the slim runtime container ships no
 * gcc/make, so an in-process `aimee git verify` there can never pass and the
 * implement block loops to its wall-cap; see docs/DELEGATE_SANDBOX_VERIFY.md).
 *
 * Reuses the docker delegate backend so verify inherits the SAME sandbox the
 * delegate used: the resolved per-project image (custom `sandbox:` spec honored
 * via delegate_sandbox_resolve_image) and the egress package proxy for first-time
 * dependency installs — and each step is fed to sandbox_learned_observe so those
 * apt installs are captured and pre-baked into the next image build (egress →
 * offline over time).
 *
 * Returns 0 and writes a {verdict, steps:[{name,rc}]} JSON to out_verdict when the
 * sandbox path ran; -1 when it is unavailable (no docker backend, no resolvable
 * verify steps, or the sandbox could not start) so the caller falls back to the
 * in-process gate — preserving CLI and non-sandboxed behavior unchanged. */
static int wfe_verify_in_sandbox(const char *workdir, char *out_verdict, size_t n)
{
   if (!workdir || !workdir[0])
      return -1;
   verify_config_t cfg;
   if (verify_load_config(workdir, &cfg) != 0 || cfg.count <= 0)
      return -1; /* no steps resolvable here -> let the caller fall back */

   delegate_backend_t *be = delegate_backend_lookup("docker");
   if (!be || !be->acquire || !be->exec || !be->release)
      return -1; /* no sandbox backend -> fall back to in-process verify */

   char image[256] = "";
   (void)delegate_sandbox_resolve_image(workdir, image, sizeof image); /* "" -> backend default */

   delegate_backend_config_t bc = {0};
   bc.image = image[0] ? image : NULL;
   bc.workspace = workdir;
   bc.pkg_proxy = 1;         /* egress proxy for first-time dep installs (learned pre-bake) */
   bc.require_isolation = 1; /* --network none; refuse to run un-isolated */
   bc.hibernate_on_exit = 1; /* keep the container + learned materials for reuse */

   /* Stable per-worktree task id so the sandbox (and its learned toolchain) is
    * reused across an implement unit's retries rather than rebuilt each round. */
   char task_id[80];
   const char *slash = strrchr(workdir, '/');
   snprintf(task_id, sizeof task_id, "wfe-verify-%s", (slash && slash[1]) ? slash + 1 : workdir);

   void *state = NULL;
   if (be->acquire(be, task_id, &bc, &state) != 0)
      return -1; /* sandbox could not start -> fall back */

   cJSON *verdict = cJSON_CreateObject();
   cJSON *steps = verdict ? cJSON_AddArrayToObject(verdict, "steps") : NULL;
   char *obuf = malloc(1 << 18), *ebuf = malloc(1 << 15);
   int failed = 0, ran = 0;
   if (verdict && steps && obuf && ebuf)
   {
      for (int i = 0; i < cfg.count; i++)
      {
         const verify_step_t *s = &cfg.steps[i];
         if (!s->run[0])
            continue;
         /* Capture any apt installs this step performs so they pre-bake into the
          * next sandbox image build (B -> A: egress-installed becomes baked-in). */
         sandbox_learned_observe(workdir, s->run);
         delegate_exec_result_t res = {0};
         res.stdout_buf = obuf;
         res.stdout_cap = 1 << 18;
         res.stderr_buf = ebuf;
         res.stderr_cap = 1 << 15;
         obuf[0] = ebuf[0] = '\0';
         int trc = be->exec(be, state, s->run, WFE_VERIFY_STEP_TIMEOUT_MS, &res);
         int step_rc = (trc != 0) ? -1 : res.exit_code; /* transport failure = step failure */
         if (step_rc != 0)
            failed++;
         ran++;
         cJSON *js = cJSON_CreateObject();
         if (js)
         {
            cJSON_AddStringToObject(js, "name", s->name);
            cJSON_AddNumberToObject(js, "rc", step_rc);
            cJSON_AddItemToArray(steps, js);
         }
      }
   }
   be->release(be, state, 1 /* hibernate: keep the sandbox for the next retry */);

   int rc = -1;
   if (verdict && steps && ran > 0)
   {
      cJSON_AddStringToObject(verdict, "verdict", failed ? "failed" : "passed");
      char *sv = cJSON_PrintUnformatted(verdict);
      if (sv)
      {
         snprintf(out_verdict, n, "%s", sv);
         free(sv);
         rc = 0;
      }
   }
   free(obuf);
   free(ebuf);
   cJSON_Delete(verdict);
   return rc;
}

/* Live verify provider (WP-1b): run the mechanical verify gate synchronously on
 * `workdir` and return the structured format=json verdict. The implement block
 * gates a unit's advance on verdict:passed. Returns 0 + fills out_verdict, or -1
 * if no verdict text came back (the block treats that as a non-pass, fail closed). */
static int wfe_live_verify_run(const char *workdir, char *out_verdict, size_t n)
{
   if (out_verdict && n)
      out_verdict[0] = '\0';
   /* Prefer the sandbox: the in-process gate below runs in the toolchain-less
    * server container and can never build a real project. Falls through to
    * in-process only when no sandbox is available (CLI / non-sandboxed setups). */
   if (wfe_verify_in_sandbox(workdir, out_verdict, n) == 0)
      return 0;
   cJSON *args = cJSON_CreateObject();
   if (!args)
      return -1;
   cJSON_AddStringToObject(args, "action", "run");
   cJSON_AddBoolToObject(args, "async", 0); /* synchronous: we need the verdict now */
   cJSON_AddStringToObject(args, "format", "json");
   /* Assert authority over the target: this provider verifies a SPECIFIC work-item
    * worktree (we pin the run_cmd CWD to it below), so it is the authoritative
    * in-process caller force_in_scope exists for. Without this, handle_git_verify
    * falls to verify_project_in_scope(), which reports the WFE repo out-of-scope
    * whenever it is not the daemon's "current project" -> an "unavailable" verdict
    * that loops the implement block to its cap. Forcing in-scope here decouples the
    * WFE verify from the global verify_cross_project flag (a deployment knob that
    * should not gate whether a slice's own committed tree can be verified). */
   cJSON_AddBoolToObject(args, "force_in_scope", 1);
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
   wfe_delegate_result_t ok = wfe_coord_task_wait(job_id, task_id, result, sizeof result, NULL, 0);
   if (ok == WFE_DELEGATE_ERROR)
      db1_coord_job_cancel(job_id);

   int refuted = 1; /* fail-closed default */
   if (ok == WFE_DELEGATE_OK && result[0])
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

void wfe_autonomy_register(void)
{
   /* Full engine executor set so a work item can run end-to-end server-side. */
   wfe_register_default_executors();
#if AIMEE_WITH_ROUNDTABLE
   wfe_register_roundtable_gate();
   /* The live roundtable panel (per-persona review delegates -> verdicts). Replaces the
    * default fail-closed stub so gate.roundtable can actually convene; still fail-closed
    * (DEGRADED/park) when a required persona has no reachable agent. */
   wfe_live_panel_register();
#endif
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
   aimee_log(LOG_INFO, "wfe", "autonomous development registered (default-on)");
}
