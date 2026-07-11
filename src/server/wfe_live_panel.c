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
#include "config.h"
#include "delegate_role.h"
#include "persona.h"
#include "provider_catalog.h"
#include "roundtable_preset.h"
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
static char *dispatch_review(const char *workdir, const char *persona, const char *prompt)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof acfg);
   if (agent_load_config(&acfg) != 0)
      return NULL;
   char *sys_prompt = persona_compose_delegate_prompt(persona, workdir, "");
   persona_t pinfo;
   memset(&pinfo, 0, sizeof pinfo);
   persona_load(NULL, persona, &pinfo);

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
   agent_result_t res;
   memset(&res, 0, sizeof res);
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
   /* No agent advertises this review persona (roles + persona-support). Fall back
    * to the configured DEFAULT ROUNDTABLE preset (roundtable.default): each seat
    * binds a persona to a concrete model, so dispatch the persona on that model's
    * agent. This lets the panel compose from the operator's configured roundtable
    * instead of failing closed to DEGRADED when the bare persona has no agent. */
   if (!chosen)
   {
      config_t cfg;
      roundtable_preset_t pr;
      if (config_load(&cfg) == 0 && cfg.roundtable_default[0] &&
          roundtable_preset_load(cfg.roundtable_default, &pr) == 0)
      {
         for (int si = 0; si < pr.seat_count && !chosen; si++)
         {
            if (strcmp(pr.seats[si].persona, persona) != 0 || !pr.seats[si].model[0])
               continue;
            for (int ai = 0; ai < acfg.agent_count; ai++)
            {
               agent_t *ag = &acfg.agents[ai];
               if (ag->enabled && strcmp(ag->name, pr.seats[si].model) == 0 &&
                   agent_is_available_for_routing(ag) &&
                   provider_catalog_get_health(ag->name) != CATALOG_HEALTH_DOWN)
               {
                  chosen = ag;
                  chosen_role =
                      delegate_role_canonicalize(pinfo.roles_count > 0 ? pinfo.roles[0] : "review");
                  break;
               }
            }
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

   /* Enforce read-only: hard-reset to the pre-review HEAD + clean untracked. */
   if (pre_head[0])
   {
      const char *reset[] = {"reset", "--hard", pre_head};
      panel_git(workdir, reset, 3);
      const char *clean[] = {"clean", "-fd"};
      panel_git(workdir, clean, 2);
   }

   char *out = NULL;
   if (rc == 0 && res.response && res.response[0])
      out = strdup(res.response);
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

   int filled = 0;
   for (int i = 0; i < nreq && filled < max; i++)
   {
      char *prompt = build_prompt(required[i], pkt);
      if (!prompt)
         continue; /* OOM building the prompt -> leave this lens unfilled (gate degrades) */
      char *resp = dispatch_review(pkt->workdir, required[i], prompt);
      free(prompt);
      if (!resp)
      {
         /* This required persona couldn't be dispatched: leave it WITHOUT a verdict so
          * the gate DEGRADES (a missing required lens must never be papered over). */
         aimee_log(LOG_WARN, "wfe-panel", "no agent for required persona '%s' -> degrade",
                   required[i]);
         continue;
      }
      wfe_panel_verdict_from_review(required[i], pkt->artifact_hash, resp, &out[filled]);
      free(resp);
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
