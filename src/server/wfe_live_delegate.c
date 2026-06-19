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
#include "log.h"
#include "util.h"
#include "wfe_approval.h"
#include "wfe_blocks.h"
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

   /* Run WITH TOOLS, pinned to the work-item worktree, so file edits land there.
    * A specific agent runs by name; otherwise route by role. max_tokens 0 =
    * model-derived cap (never under-cap a reasoning delegate). */
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
         if (err && errlen)
            snprintf(err, errlen, "wfe live delegate: unknown delegate '%s'", agent_name);
         return -1;
      }
      rc = agent_execute_with_tools(ag, NULL, "", prompt, AGENT_DEFAULT_MAX_TOKENS, 0.3, &res);
   }
   else
   {
      rc = agent_run_with_tools(&acfg, role, "", prompt, AGENT_DEFAULT_MAX_TOKENS, &res);
   }
   run_cmd_set_cwd(NULL);

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

void wfe_autonomy_register(void)
{
   /* Full engine executor set so a work item can run end-to-end server-side. */
   wfe_register_default_executors();
   wfe_register_roundtable_gate();
   wfe_register_human_gate();
   /* The live worker. The forge `open` provider stays the default fail-closed
    * stub for now (pr.open re-loops until real git push + PR wiring lands), which
    * is safe. */
   wfe_set_delegate_provider(&WFE_LIVE_DELEGATE);
   aimee_log(LOG_INFO, "wfe", "autonomous development registered (default-on)");
}
