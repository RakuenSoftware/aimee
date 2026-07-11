/* wfe_live_panel.c -- the live roundtable panel provider.
 *
 * gate.roundtable calls this to convene a diverse panel: it dispatches ONE read-only
 * review delegate per REQUIRED persona (the same synchronous, worktree-reset-enforced
 * mechanism the implement adversarial judge uses), then maps each reply to a verdict
 * (wfe_panel_verdict_from_review). A persona that cannot be dispatched is left WITHOUT
 * a verdict so wfe_gate_decide DEGRADES (parks) rather than approving on a partial
 * panel — fail closed. Registered from wfe_autonomy_register.
 *
 * NOTE: the per-persona dispatch requires reachable review agents, so it is exercised
 * by integration (a live deployment), not the unit suite; the risk-bearing mapping is
 * unit-tested in test_wfe_panel_verdict. The panel is inert until a run reaches a
 * gate.roundtable node, so registration alone changes nothing.
 */
#include "aimee.h"

#include "wfe_panel_verdict.h"
#include "wfe_roundtable.h"

#include "agent.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "delegate_role.h"
#include "persona.h"
#include "provider_catalog.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run `git -C workdir <args...>`, discard output. */
static void panel_git(const char *workdir, const char *const extra[], int extra_n)
{
   const char *argv[16];
   int argc = 0;
   argv[argc++] = "git";
   argv[argc++] = "-C";
   argv[argc++] = workdir;
   for (int i = 0; i < extra_n && argc < 15; i++)
      argv[argc++] = extra[i];
   argv[argc] = NULL;
   char *o = NULL;
   (void)safe_exec_capture(argv, &o, 1 << 14);
   free(o);
}

/* Max panel seats (matches the verdict-array bound the gate.roundtable executor
 * passes as `max`). */
#define WFE_PANEL_MAX 16
/* Per-round wall-clock ceiling for the parallel panel: a panelist still running
 * at the deadline is abandoned (its lens left unfilled) so one hung model can
 * never wedge the round. */
#define WFE_PANEL_DEADLINE_MS 300000

/* Build the per-persona review prompt. The change under review is embedded directly
 * as a diff so the panelist reads the delta and navigates surrounding code via the
 * aimee index (find_symbol/search_memory) instead of re-running git and sweeping the
 * worktree. Returns a malloc'd prompt sized to fit the diff (caller frees), or NULL
 * on allocation failure. */
static char *build_prompt(const char *persona, const wfe_review_packet_t *pkt)
{
   const char *focus =
       (pkt->focus && pkt->focus[0]) ? pkt->focus : "correctness, quality, and completeness";
   const char *proposal = (pkt->proposal && pkt->proposal[0]) ? pkt->proposal : "(none provided)";
   const char *diff = (pkt->diff && pkt->diff[0])
                          ? pkt->diff
                          : "(no code diff — review the plan/proposal artifact against the ask)";
   size_t cap = 2048 + strlen(focus) + strlen(proposal) + strlen(diff);
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   snprintf(buf, cap,
            "You are the %s lens on a review roundtable. Review the CHANGE UNDER REVIEW below "
            "AGAINST what was asked. Do NOT edit files, and do NOT sweep or re-read the repo to "
            "reconstruct context: trust aimee's index/graph (find_symbol, search_memory) as the "
            "authoritative source for the current codebase, and open a file only to confirm what "
            "the index cannot resolve.\n\nFOCUS: %s\n\nORIGINAL PROPOSAL/REQUEST:\n%.4000s\n\n"
            "CHANGE UNDER REVIEW (diff vs the base repo):\n%s\n\n"
            "End your reply with EXACTLY one JSON line and nothing after it: "
            "{\"verdict\":\"approve\"} if it satisfies the ask with no high-severity blocker, "
            "{\"verdict\":\"request_changes\",\"high_sev_blockers\":<N>} if there is a real "
            "blocker, or {\"verdict\":\"comment\"} if you only have non-blocking remarks.",
            persona, focus, proposal, diff);
   return buf;
}

/* Compose the review roundtable IN PARALLEL: assign a DISTINCT viable `review`
 * delegate to each required lens (persona rides in the system prompt as the lens;
 * selection is purely by the `review` role, so non-review agents like gpu-mid are
 * never seated), then dispatch them ALL concurrently via agent_run_parallel
 * (bounded by the compute-thread ceiling; a panelist past the deadline is
 * abandoned). A second parallel round retries any lens whose agent failed on a
 * different agent, so a flaky agent doesn't degrade the gate. Reviews are
 * read-only (the `review` role grants no write tools); one hard-reset after the
 * panel defends the worktree regardless. Returns the number of lenses filled. */
static int live_panel(const wfe_review_packet_t *pkt, const char *const *required, int nreq,
                      const char *const *eligible, int nelig, wfe_verdict_t *out, int max)
{
   (void)eligible;
   (void)nelig;
   if (!pkt || !pkt->workdir || !pkt->workdir[0])
      return -1; /* no worktree to review in -> panel can't compose -> park */

   agent_config_t acfg;
   memset(&acfg, 0, sizeof acfg);
   if (agent_load_config(&acfg) != 0)
      return -1;

   int nlens = nreq < max ? nreq : max;
   if (nlens > WFE_PANEL_MAX)
      nlens = WFE_PANEL_MAX;

   /* Per-lens prompts: the persona system prompt is the review lens; the user
    * prompt embeds the change under review. */
   char *sysp[WFE_PANEL_MAX];
   char *usrp[WFE_PANEL_MAX];
   int done[WFE_PANEL_MAX];
   memset(sysp, 0, sizeof sysp);
   memset(usrp, 0, sizeof usrp);
   memset(done, 0, sizeof done);
   for (int i = 0; i < nlens; i++)
   {
      sysp[i] = persona_compose_delegate_prompt(required[i], pkt->workdir, "");
      usrp[i] = build_prompt(required[i], pkt);
   }

   /* Snapshot HEAD once for the post-panel read-only reset. */
   char pre_head[64] = "";
   {
      const char *argv[] = {"git", "-C", pkt->workdir, "rev-parse", "HEAD", NULL};
      char *o = NULL;
      if (safe_exec_capture(argv, &o, 256) == 0 && o)
      {
         size_t n = strlen(o);
         while (n > 0 && (o[n - 1] == '\n' || o[n - 1] == '\r'))
            o[--n] = '\0';
         snprintf(pre_head, sizeof pre_head, "%s", o);
      }
      free(o);
   }

   run_cmd_set_cwd(pkt->workdir);

   const char *used[MAX_AGENTS];
   int nused = 0;
   int filled = 0;
   /* Up to two parallel rounds: assign distinct agents to the still-unfilled
    * lenses, fan out concurrently, then retry the failures once on other agents. */
   for (int round = 0; round < 2 && filled < nlens; round++)
   {
      agent_task_t tasks[WFE_PANEL_MAX];
      int lensmap[WFE_PANEL_MAX];
      memset(tasks, 0, sizeof tasks);
      int nt = 0;
      for (int i = 0; i < nlens; i++)
      {
         if (done[i])
            continue;
         int idx = delegate_pick_for_role(&acfg, "review", used, nused);
         if (idx < 0)
            break; /* no more distinct review agents remain */
         if (nused < MAX_AGENTS)
            used[nused++] = acfg.agents[idx].name;
         tasks[nt].role = "review";
         tasks[nt].agent = acfg.agents[idx].name;
         tasks[nt].system_prompt = sysp[i] ? sysp[i] : "";
         tasks[nt].user_prompt = usrp[i] ? usrp[i] : "";
         tasks[nt].temperature = 0.2;
         tasks[nt].max_tokens = AGENT_DEFAULT_MAX_TOKENS;
         lensmap[nt] = i;
         nt++;
      }
      if (nt == 0)
         break;

      agent_result_t results[WFE_PANEL_MAX];
      memset(results, 0, sizeof results);
      agent_run_parallel(&acfg, tasks, nt, results, WFE_PANEL_DEADLINE_MS);
      for (int t = 0; t < nt; t++)
      {
         int i = lensmap[t];
         if (results[t].response && results[t].response[0])
         {
            wfe_panel_verdict_from_review(required[i], pkt->artifact_hash, results[t].response,
                                          &out[filled]);
            filled++;
            done[i] = 1;
         }
         free(results[t].response);
      }
   }

   run_cmd_set_cwd(NULL);

   /* Read-only enforcement: hard-reset to the pre-panel HEAD + clean untracked. */
   if (pre_head[0])
   {
      const char *reset[] = {"reset", "--hard", pre_head};
      panel_git(pkt->workdir, reset, 3);
      const char *clean[] = {"clean", "-fd"};
      panel_git(pkt->workdir, clean, 2);
   }

   for (int i = 0; i < nlens; i++)
   {
      if (!done[i])
         aimee_log(LOG_WARN, "wfe-panel", "no viable review agent for lens '%s' -> degrade",
                   required[i]);
      free(sysp[i]);
      free(usrp[i]);
   }
   return filled;
}

void wfe_live_panel_register(void)
{
   wfe_set_panel_provider(live_panel);
   aimee_log(LOG_INFO, "wfe-panel", "live roundtable panel provider registered");
}
