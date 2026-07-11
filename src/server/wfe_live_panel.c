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

/* Dispatch persona `persona` as a READ-ONLY reviewer in `workdir` with `prompt`.
 * Returns the agent's response (malloc'd, caller frees), or NULL if no eligible agent
 * could run. Any edit the reviewer makes is discarded (hard reset) — read-only is
 * enforced, not merely requested. */
static char *dispatch_review(const char *workdir, const char *persona, const char *prompt,
                             const char *const used[], int nused, char *out_agent,
                             size_t out_agent_n)
{
   if (out_agent && out_agent_n)
      out_agent[0] = '\0';
   agent_config_t acfg;
   memset(&acfg, 0, sizeof acfg);
   if (agent_load_config(&acfg) != 0)
      return NULL;
   char *sys_prompt = persona_compose_delegate_prompt(persona, workdir, "");

   char pre_head[64] = "";
   {
      const char *argv[] = {"git", "-C", workdir, "rev-parse", "HEAD", NULL};
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

   run_cmd_set_cwd(workdir);

   /* Ask for a viable `review` delegate. The persona rides in the system prompt as
    * the review lens; agent selection is purely by the `review` role, so non-review
    * agents (e.g. gpu-mid, which lacks the role) are never picked. Exclude agents
    * already seated on this panel (diversity) and any that fail this round, and
    * retry until one lands — a flaky agent no longer degrades the whole gate. */
   const char *tried[MAX_AGENTS];
   int ntried = 0;
   for (int i = 0; i < nused && ntried < MAX_AGENTS; i++)
      tried[ntried++] = used[i];

   agent_result_t res;
   memset(&res, 0, sizeof res);
   agent_t *chosen = NULL;
   int rc = -1;
   while (ntried < MAX_AGENTS)
   {
      int idx = delegate_pick_for_role(&acfg, "review", tried, ntried);
      if (idx < 0)
         break;
      chosen = &acfg.agents[idx];
      memset(&res, 0, sizeof res);
      rc = agent_execute_with_tools_for_role(chosen, &acfg.network, "review",
                                             sys_prompt ? sys_prompt : "", prompt,
                                             AGENT_DEFAULT_MAX_TOKENS, 0.2, &res);
      if (rc == 0 && res.response && res.response[0])
         break;
      provider_catalog_record_failure(chosen->name,
                                      agent_error_is_retryable(res.error) ? "retryable" : "error");
      free(res.response);
      memset(&res, 0, sizeof res);
      tried[ntried++] = chosen->name;
      chosen = NULL;
      rc = -1;
   }

   run_cmd_set_cwd(NULL);
   free(sys_prompt);

   /* Enforce read-only: hard-reset to the pre-review HEAD + clean untracked. */
   if (pre_head[0])
   {
      const char *reset[] = {"reset", "--hard", pre_head};
      panel_git(workdir, reset, 3);
      const char *clean[] = {"clean", "-fd"};
      panel_git(workdir, clean, 2);
   }

   char *out = NULL;
   if (rc == 0 && chosen && res.response && res.response[0])
   {
      out = strdup(res.response);
      provider_catalog_record_success(chosen->name);
      if (out_agent && out_agent_n)
         snprintf(out_agent, out_agent_n, "%s", chosen->name);
   }
   free(res.response);
   return out;
}

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

static int live_panel(const wfe_review_packet_t *pkt, const char *const *required, int nreq,
                      const char *const *eligible, int nelig, wfe_verdict_t *out, int max)
{
   (void)eligible;
   (void)nelig;
   if (!pkt || !pkt->workdir || !pkt->workdir[0])
      return -1; /* no worktree to review in -> panel can't compose -> park */

   /* Distinct agent per seated lens: each review delegate excludes the agents
    * already used on this panel, so the roundtable is a genuinely diverse panel. */
   char used_buf[MAX_AGENTS][MAX_AGENT_NAME];
   const char *used[MAX_AGENTS];
   int nused = 0;

   int filled = 0;
   for (int i = 0; i < nreq && filled < max; i++)
   {
      char *prompt = build_prompt(required[i], pkt);
      if (!prompt)
         continue; /* OOM building the prompt -> leave this lens unfilled (gate degrades) */
      char agent_name[MAX_AGENT_NAME] = "";
      char *resp = dispatch_review(pkt->workdir, required[i], prompt, used, nused, agent_name,
                                   sizeof agent_name);
      free(prompt);
      if (!resp)
      {
         /* No viable review agent remains for this lens: leave it WITHOUT a verdict
          * so the gate DEGRADES (a missing required lens must never be papered over). */
         aimee_log(LOG_WARN, "wfe-panel", "no viable review agent for lens '%s' -> degrade",
                   required[i]);
         continue;
      }
      wfe_panel_verdict_from_review(required[i], pkt->artifact_hash, resp, &out[filled]);
      free(resp);
      if (agent_name[0] && nused < MAX_AGENTS)
      {
         snprintf(used_buf[nused], sizeof used_buf[0], "%s", agent_name);
         used[nused] = used_buf[nused];
         nused++;
      }
      filled++;
   }
   /* filled==0 (nothing composed) -> the gate sees no required verdicts -> DEGRADED. */
   return filled;
}

void wfe_live_panel_register(void)
{
   wfe_set_panel_provider(live_panel);
   aimee_log(LOG_INFO, "wfe-panel", "live roundtable panel provider registered");
}
