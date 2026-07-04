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
#include "agent_types.h"
#include "cJSON.h"
#include "delegate_role.h"
#include "persona.h"
#include "provider_catalog.h"
#include "headers/git_verify.h"
#include "log.h"
#include "util.h"
#include "wfe_approval.h"
#include "wfe_blocks.h"
#include "wfe_live_forge.h"
#include "wfe_roundtable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void chomp(char *s)
{
   if (!s)
      return;
   size_t n = strlen(s);
   while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
      s[--n] = '\0';
}

/* Run `git -C workdir <args...>`, discard output. Returns the exec rc. */
static int git_run(const char *workdir, const char *const extra[], int extra_n)
{
   const char *argv[16];
   int argc = 0;
   argv[argc++] = "git";
   argv[argc++] = "-C";
   argv[argc++] = workdir;
   for (int i = 0; i < extra_n && argc < 15; i++)
      argv[argc++] = extra[i];
   argv[argc] = NULL;
   char *out = NULL;
   int rc = safe_exec_capture(argv, &out, 1 << 16);
   free(out);
   return rc;
}

/* wfe_resolve_delegate + the $random selector live in wfe_delegate_resolve.c
 * (self-contained, unit-tested). */

/* The live delegate run. Contract per wfe_delegate_provider_t. */
static int wfe_live_delegate_run(const char *workdir, const char *role, const char *delegate,
                                 const char *prompt, const char *artifact_path,
                                 char out_commit_sha[64], char *err, size_t errlen)
{
   if (out_commit_sha)
      out_commit_sha[0] = '\0';
   if (!workdir || !workdir[0] || !prompt || !prompt[0] ||
       ((!role || !role[0]) && (!delegate || !delegate[0])))
   {
      if (err && errlen)
         snprintf(err, errlen, "wfe live delegate: missing workdir/prompt and role+delegate");
      return -1;
   }

   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   if (agent_load_config(&acfg) != 0)
   {
      if (err && errlen)
         snprintf(err, errlen, "wfe live delegate: agent config load failed");
      return -1;
   }

   /* Resolve the step's delegate ("$random" -> a random roster agent; "" -> route
    * by role). Fail fast rather than leak the sentinel downstream. */
   char agent_name[MAX_AGENT_NAME] = "";
   if (wfe_resolve_delegate(delegate, &acfg, agent_name, sizeof agent_name) != 0)
   {
      if (err && errlen)
         snprintf(err, errlen, "wfe live delegate: no enabled agent for '$random'");
      return -1;
   }

   /* The producing blocks pass a PERSONA (engineer / architect / ...) — the
    * delegate's IDENTITY — in this slot, NOT an agent routing role. (It was
    * historically misused as a role, so no agent matched — author/implement
    * looped forever.) Compose the persona's identity + principles as the system
    * prompt, and route by a ROLE the persona is allowed to use that an eligible
    * agent can serve. */
   const char *persona = (role && role[0]) ? role : "engineer";
   char *sys_prompt = persona_compose_delegate_prompt(persona, workdir, "");
   persona_t pinfo;
   memset(&pinfo, 0, sizeof pinfo);
   persona_load(NULL, persona, &pinfo);

   /* Run WITH TOOLS, pinned to the work-item worktree, so file edits land there.
    * A specific agent runs by name; otherwise route by persona. */
   run_cmd_set_cwd(workdir);
   agent_result_t res;
   memset(&res, 0, sizeof(res));
   int rc;
   if (agent_name[0])
   {
      agent_t *ag = agent_find(&acfg, agent_name);
      if (!ag)
      {
         run_cmd_set_cwd(NULL);
         free(sys_prompt);
         persona_free(&pinfo);
         if (err && errlen)
            snprintf(err, errlen, "wfe live delegate: unknown delegate '%s'", agent_name);
         return -1;
      }
      rc = agent_execute_with_tools(ag, &acfg.network, sys_prompt ? sys_prompt : "", prompt,
                                    AGENT_DEFAULT_MAX_TOKENS, 0.3, &res);
   }
   else
   {
      /* Route by the persona's allowed roles (ordered by preference): for the
       * first role that ANY healthy, persona-eligible agent serves, pick the
       * lowest-cost-tier such agent. Scanning every agent per role (not just
       * agent_route's single best) means a persona-ineligible best-tier agent
       * can't shadow an eligible one that serves the same role. */
      agent_t *chosen = NULL;
      const char *chosen_role = NULL;
      int best_tier = 0;
      for (int ri = 0; ri < pinfo.roles_count && !chosen; ri++)
      {
         const char *r = delegate_role_canonicalize(pinfo.roles[ri]);
         for (int ai = 0; ai < acfg.agent_count; ai++)
         {
            agent_t *ag = &acfg.agents[ai];
            if (!ag->enabled || !agent_has_role(ag, r) || !agent_supports_persona(ag, persona) ||
                !agent_is_available_for_routing(ag))
               continue;
            if (provider_catalog_get_health(ag->name) == CATALOG_HEALTH_DOWN)
               continue;
            if (!chosen || ag->cost_tier < best_tier)
            {
               chosen = ag;
               chosen_role = r;
               best_tier = ag->cost_tier;
            }
         }
      }
      if (!chosen)
      {
         run_cmd_set_cwd(NULL);
         free(sys_prompt);
         persona_free(&pinfo);
         if (err && errlen)
            snprintf(err, errlen, "wfe live delegate: no enabled agent eligible for persona '%s'",
                     persona);
         return -1;
      }
      rc = agent_execute_with_tools_for_role(chosen, &acfg.network, chosen_role,
                                             sys_prompt ? sys_prompt : "", prompt,
                                             AGENT_DEFAULT_MAX_TOKENS, 0.3, &res);
   }
   run_cmd_set_cwd(NULL);
   free(sys_prompt);
   persona_free(&pinfo);

   if (rc != 0)
   {
      if (err && errlen)
         snprintf(err, errlen, "%s", res.error[0] ? res.error : "delegate run failed");
      free(res.response);
      return -1;
   }

   /* Text-artifact blocks (e.g. author.proposal/plan): if the agent produced a
    * response and did not itself write the artifact file, persist the response so
    * the following gate has content to act on. Code blocks edit files via tools
    * and usually leave artifact_path NULL. */
   if (artifact_path && artifact_path[0] && res.response && res.response[0])
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
            fwrite(res.response, 1, strlen(res.response), wf);
            fclose(wf);
         }
      }
   }
   free(res.response);

   /* Stage + commit whatever the delegate changed in the worktree. A commit with
    * nothing staged is a harmless no-op (rc ignored); out_commit_sha is then just
    * the unchanged HEAD. NO co-authorship trailer (standing directive). */
   {
      const char *add[] = {"add", "-A"};
      (void)git_run(workdir, add, 2);
      const char *commit[] = {
          "-c",     "user.name=aimee", "-c", "user.email=aimee@localhost",
          "commit", "--no-verify",     "-m", "aimee: autonomous delegate change"};
      (void)git_run(workdir, commit, 8);
   }
   {
      const char *argv[] = {"git", "-C", workdir, "rev-parse", "HEAD", NULL};
      char *o = NULL;
      if (safe_exec_capture(argv, &o, 256) == 0 && o)
      {
         chomp(o);
         if (out_commit_sha)
            snprintf(out_commit_sha, 64, "%s", o);
      }
      free(o);
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
   if (workdir && workdir[0])
      cJSON_AddStringToObject(args, "path", workdir);
   cJSON *resp = handle_git_verify(NULL, args, NULL);
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

   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   if (agent_load_config(&acfg) != 0)
      return -1;
   char *sys_prompt = persona_compose_delegate_prompt(persona, workdir, "");
   persona_t pinfo;
   memset(&pinfo, 0, sizeof pinfo);
   persona_load(NULL, persona, &pinfo);

   /* Read-only enforcement (defense in depth): capture the pre-judge HEAD so we can
    * hard-reset the worktree afterwards — the judge is instructed not to edit, but we
    * do not rely on instruction-following; any edit it makes is discarded. */
   char pre_head[64] = "";
   {
      const char *argv[] = {"git", "-C", workdir, "rev-parse", "HEAD", NULL};
      char *o = NULL;
      if (safe_exec_capture(argv, &o, 256) == 0 && o)
      {
         chomp(o);
         snprintf(pre_head, sizeof pre_head, "%s", o);
      }
      free(o);
   }

   run_cmd_set_cwd(workdir);
   agent_result_t res;
   memset(&res, 0, sizeof(res));
   agent_t *chosen = NULL;
   const char *chosen_role = NULL;
   int best_tier = 0;
   for (int ri = 0; ri < pinfo.roles_count && !chosen; ri++)
   {
      const char *r = delegate_role_canonicalize(pinfo.roles[ri]);
      for (int ai = 0; ai < acfg.agent_count; ai++)
      {
         agent_t *ag = &acfg.agents[ai];
         if (!ag->enabled || !agent_has_role(ag, r) || !agent_supports_persona(ag, persona) ||
             !agent_is_available_for_routing(ag))
            continue;
         if (provider_catalog_get_health(ag->name) == CATALOG_HEALTH_DOWN)
            continue;
         if (!chosen || ag->cost_tier < best_tier)
         {
            chosen = ag;
            chosen_role = r;
            best_tier = ag->cost_tier;
         }
      }
   }
   int rc = -1;
   if (chosen)
      rc = agent_execute_with_tools_for_role(chosen, &acfg.network, chosen_role,
                                             sys_prompt ? sys_prompt : "", prompt,
                                             AGENT_DEFAULT_MAX_TOKENS, 0.2, &res);
   run_cmd_set_cwd(NULL);
   free(sys_prompt);
   persona_free(&pinfo);

   /* Discard anything the judge changed: hard-reset to the pre-judge HEAD + clean
    * untracked. This makes the read-only property enforced, not merely requested. */
   if (pre_head[0])
   {
      const char *reset[] = {"reset", "--hard", pre_head};
      (void)git_run(workdir, reset, 3);
      const char *clean[] = {"clean", "-fd"};
      (void)git_run(workdir, clean, 2);
   }

   int refuted = 1; /* fail-closed default */
   if (rc == 0 && res.response && res.response[0])
   {
      /* Parse ONLY the LAST non-empty line as the verdict JSON (the delegate is told
       * to emit exactly one JSON line at the end). A substring scan of the whole
       * response would false-ACCEPT on a skeptic's reasoning that quotes
       * {"refuted": false}. Accept only on an explicit boolean refuted:false; anything
       * that doesn't parse to that is REFUTED (fail closed). */
      const char *r = res.response;
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
   free(res.response);
   if (rc != 0 && !chosen)
      snprintf(out_verdict, n, "{\"refuted\":true,\"reason\":\"no judge agent available\"}");
   else
      snprintf(out_verdict, n, "{\"refuted\":%s}", refuted ? "true" : "false");
   return 0; /* a verdict was produced (even a fail-closed one) */
}

static const wfe_judge_provider_t WFE_LIVE_JUDGE = {wfe_live_judge_run};

void wfe_autonomy_register(void)
{
   /* Full engine executor set so a work item can run end-to-end server-side. */
   wfe_register_default_executors();
   wfe_register_roundtable_gate();
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
   aimee_log(LOG_INFO, "wfe", "autonomous development registered (default-on)");
}
