/* wfe_live_panel.c -- the live roundtable panel provider.
 *
 * gate.roundtable calls this to convene a diverse panel THROUGH THE ROUNDTABLE
 * ENGINE (delegate_roundtable_run, REVIEW mode): the exact seats from the
 * named/default configured roundtable (or at most two fallback agents), whose findings are captured
 * as review items with replayable evidence, deduped across the panel, replay-VERIFIED against the
 * gate's worktree (wfe_replay_worktree — interpretation never blocks, a
 * contradicted claim is rejected), and finally mapped onto per-lens verdicts
 * (wfe_panel_verdicts_from_roundtable) for the fail-closed wfe_gate_decide.
 * Registered from wfe_autonomy_register.
 *
 * A configured seat model is never silently substituted; when no review agent
 * is eligible right now the panel QUEUES for a seat up to
 * AIMEE_PANEL_SEAT_WAIT_SECS before the gate degrades.
 *
 * NOTE: the engine embeds the change in the prompt, and panelists additionally
 * carry aimee's INDEX-ONLY review toolset (`review_indexed`: code_search /
 * find_symbol / search_memory / search_docs) so they can check what the diff
 * cannot show — callers, writers, whether a named alternative exists. Still no
 * filesystem or write tools: the panel does not touch the worktree (a remote seat
 * may not reach it, and a reviewer must not edit what it judges); only the
 * verification pass reads it. Dispatch requires reachable review agents, so this provider is
 * exercised by integration (a live deployment); the risk-bearing pieces — the
 * verdict mapping and the worktree replay backend — are unit-tested in
 * test_wfe_panel_roundtable / test_wfe_replay_worktree. */
#include "aimee.h"

#include "wfe_panel_roundtable.h"
#include "wfe_replay_worktree.h"
#include "wfe_roundtable.h"

#include "agent_config.h"
#include "config.h"
#include "log.h"
#include "roundtable_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Max panel seats (matches the verdict-array bound the gate.roundtable executor
 * passes as `max`). */
#define WFE_PANEL_MAX 16
/* Per-attempt wall-clock ceiling for the parallel panel: a panelist still
 * running at the deadline is abandoned so one hung model can never wedge the
 * round. */
#define WFE_PANEL_DEADLINE_MS 300000

/* How long an unseatable/failed panel QUEUES for review agents before the gate
 * degrades. The review roster is small and shared with implement delegates:
 * under a parallel fleet, "no eligible review agent right now" is usually
 * transient — waiting out the contention converts an instant panel_degraded
 * park into a completed panel. 0 disables queueing (instant-degrade). */
static long wfe_panel_seat_wait_secs(void)
{
   const char *v = getenv("AIMEE_PANEL_SEAT_WAIT_SECS");
   if (v && v[0])
   {
      char *end = NULL;
      long s = strtol(v, &end, 10);
      if (end && *end == '\0' && s >= 0 && s <= 3600)
         return s;
   }
   return 300;
}

#define WFE_PANEL_SEAT_POLL_SECS 15

/* The engine review task: what was asked, the change under review, and the standing
 * questions the diff alone cannot answer. Panelists carry aimee's index-only
 * toolset, so the diff is the SUBJECT rather than the whole of the material — they
 * look up callers, writers and alternatives themselves. The engine's own REVIEW
 * round instruction supplies the structured-items output contract
 * (severity/category/location/summary/recommendation + replayable evidence). */
static char *build_review_task(const wfe_review_packet_t *pkt)
{
   static const char *fmt =
       "Review the CHANGE UNDER REVIEW below AGAINST the ORIGINAL REQUEST below.\n\n"
       "FOCUS: %s\n\nORIGINAL REQUEST:\n%s\n\n"
       "CHANGE UNDER REVIEW (diff vs the base repo):\n%s\n\n"
       "You have aimee's tools (code_search, find_symbol, search_memory, search_docs). "
       "The diff shows what CHANGED; it does not show whether the change is REAL. Look "
       "the rest up — do not infer it from the diff:\n"
       "1. REACHABLE: for new behaviour, especially a guard/gate/check, find its callers. "
       "Name the path from a real entrypoint to this code in the artifact that actually "
       "ships. Code with no caller, or whose only caller needs a binary or config the "
       "deployment lacks, is inert — that is a blocking defect no matter how correct the "
       "code reads.\n"
       "2. PRODUCT: every added file must be something we ship. Search for what writes it. "
       "Run bookkeeping, scope/intent records and scratch files are not deliverables.\n"
       "3. ALTERNATIVE EXISTS: if the change forbids or removes a way of doing something, "
       "confirm the replacement it points people to actually exists and works on the "
       "surface it targets. A rule with no working alternative is breakage.\n\n"
       "For every item, location is \"file:line\" from the change wherever possible, and a "
       "blocking severity REQUIRES reproducible factual evidence about this code.";
   const char *focus =
       (pkt->focus && pkt->focus[0]) ? pkt->focus : "correctness, quality, and completeness";
   const char *proposal = (pkt->proposal && pkt->proposal[0]) ? pkt->proposal : "(none provided)";
   const char *diff = (pkt->diff && pkt->diff[0])
                          ? pkt->diff
                          : "(no code diff — review the plan/proposal artifact against the ask)";
   /* The former buffer already used strlen() of the complete request and diff,
    * despite formatting only 4000 request bytes. Exact sizing therefore reduces
    * allocation while making silent content truncation impossible. */
   int needed = snprintf(NULL, 0, fmt, focus, proposal, diff);
   if (needed < 0)
      return NULL;
   size_t cap = (size_t)needed + 1;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   if (snprintf(buf, cap, fmt, focus, proposal, diff) != needed)
   {
      free(buf);
      return NULL;
   }
   return buf;
}

/* Convene the review roundtable through the engine and map the verified items
 * to per-lens verdicts. Returns the number of lenses filled (nlens on success,
 * 0 when the panel degrades so the gate parks) or a WFE_PANEL_* sentinel. */
static int live_panel(const wfe_review_packet_t *pkt, const char *const *required, int nreq,
                      const char *const *eligible, int nelig, wfe_verdict_t *out, int max)
{
   (void)eligible;
   (void)nelig;
   if (!pkt || !pkt->workdir || !pkt->workdir[0])
      return WFE_PANEL_UNREACHABLE; /* no worktree to review in -> park */

   agent_config_t acfg;
   memset(&acfg, 0, sizeof acfg);
   if (agent_load_config(&acfg) != 0)
      return WFE_PANEL_UNREACHABLE;

   int nlens = nreq < max ? nreq : max;
   if (nlens > WFE_PANEL_MAX)
      nlens = WFE_PANEL_MAX;

   char *task = build_review_task(pkt);
   if (!task)
      return WFE_PANEL_UNREACHABLE;

   /* Deadline-bounded attempts: re-resolve seats and re-run the panel while
    * seats are unseatable or panelists fail, until the seat-wait deadline. */
   struct timespec q0;
   clock_gettime(CLOCK_MONOTONIC, &q0);
   long seat_wait = wfe_panel_seat_wait_secs();
   int queued_logged = 0;
   int rc_final = 0;

   for (;;)
   {
      struct timespec qn;
      clock_gettime(CLOCK_MONOTONIC, &qn);
      int final = (qn.tv_sec - q0.tv_sec) >= seat_wait || seat_wait == 0;

      if (!config_present() || agent_load_config(&acfg) != 0)
      {
         if (final)
         {
            rc_final = WFE_PANEL_UNREACHABLE;
            break;
         }
         struct timespec nap = {WFE_PANEL_SEAT_POLL_SECS, 0};
         nanosleep(&nap, NULL);
         continue;
      }

      ensemble_panel_t panel;
      ensemble_panel_from_config(&panel);
      char panel_err[256];
      if (ensemble_prepare_runtime_panel(pkt->roundtable, &panel, &acfg, panel_err,
                                         sizeof panel_err) != 0)
      {
         if (final)
         {
            aimee_log(LOG_WARN, "wfe-panel", "%s -> park", panel_err);
            rc_final = (pkt->roundtable && pkt->roundtable[0]) || config_roundtable_default()[0]
                           ? WFE_PANEL_PINNED_FAIL
                           : 0;
            break;
         }
         if (!queued_logged)
         {
            aimee_log(LOG_INFO, "wfe-panel", "%s; queueing up to %lds", panel_err, seat_wait);
            queued_logged = 1;
         }
         struct timespec nap = {WFE_PANEL_SEAT_POLL_SECS, 0};
         nanosleep(&nap, NULL);
         continue;
      }

      /* Read the seats off the PANEL, and apply the two overrides to the PANEL —
       * it is what delegate_roundtable_run consumes. Both used to be written to a
       * local config_t that was passed on; once the engine took an
       * ensemble_panel_t built earlier in this loop, writing them to the config_t
       * became a pair of dead stores that silently dropped the min-successful and
       * replay-verify overrides. */
      const int panel_count = panel.reference_count;
      const char *lens_seat[WFE_PANEL_MAX];
      for (int i = 0; i < nlens; i++)
         lens_seat[i] = panel.reference_models[i % panel_count];

      /* Acquired roundtable seats are the whole panel. Review lenses are verdict
       * dimensions, not authorization to add more agents; attribute each lens
       * to an actual panel seat round-robin. */
      panel.min_successful = panel_count;
      panel.replay_verify_enabled = 0;

      roundtable_opts_t opts;
      memset(&opts, 0, sizeof opts);
      opts.mode = ROUNDTABLE_REVIEW;
      opts.turns = ROUNDTABLE_PARALLEL;
      opts.max_rounds = 1;
      opts.deadline_ms = WFE_PANEL_DEADLINE_MS;
      opts.required_participants = panel_count;

      roundtable_result_t rt;
      memset(&rt, 0, sizeof rt);
      const char *previous_cwd = run_cmd_get_cwd();
      char previous_cwd_copy[MAX_PATH_LEN];
      snprintf(previous_cwd_copy, sizeof previous_cwd_copy, "%s", previous_cwd ? previous_cwd : "");
      run_cmd_set_cwd(pkt->workdir);
      int roundtable_rc = delegate_roundtable_run(&acfg, &panel, task, &opts, &rt);
      run_cmd_set_cwd(previous_cwd_copy[0] ? previous_cwd_copy : NULL);
      if (roundtable_rc != 0)
      {
         delegate_roundtable_result_free(&rt);
         rc_final = WFE_PANEL_UNREACHABLE;
         break;
      }

      if (rt.participants_required_failed > 0 || rt.degraded)
      {
         aimee_log(LOG_WARN, "wfe-panel",
                   "panel attempt: %d/%d required and %d/%d total panelist(s) failed%s%s",
                   rt.participants_required_failed, panel_count, rt.participants_failed,
                   rt.participants_total, rt.degraded ? " (degraded)" : "",
                   final ? " -> degrade" : " -> re-seat and retry");
         delegate_roundtable_result_free(&rt);
         if (final)
         {
            rc_final = 0; /* missing lens coverage -> gate parks (fail closed) */
            break;
         }
         struct timespec nap = {WFE_PANEL_SEAT_POLL_SECS, 0};
         nanosleep(&nap, NULL);
         continue;
      }

      /* Replay-verify the deduped panel items against the worktree the panel
       * reviewed: interpretation caps below blocking, contradicted claims are
       * rejected — the same rule as the compute roundtable, re-grounded. */
      wfe_replay_worktree_set_root(pkt->workdir);
      roundtable_verify_items_with(&rt, wfe_replay_worktree_backend(), panel.require_evidence);
      wfe_replay_worktree_set_root(NULL);
      aimee_log(LOG_INFO, "wfe-panel",
                "panel items: %d kept (verified=%d capped=%d degraded=%d), %d rejected",
                rt.item_count, rt.verified_count, rt.capped_count, rt.degraded_count,
                rt.rejected_count);
      for (int i = 0; i < rt.rejected_count && i < ROUNDTABLE_MAX_REVIEW_ITEMS; i++)
         aimee_log(LOG_WARN, "wfe-panel", "rejected finding (%s) [%s] %s: %s",
                   rt.rejected_reason[i], rt.rejected[i].sources, rt.rejected[i].location,
                   rt.rejected[i].summary);

      int filled = wfe_panel_verdicts_from_roundtable(&rt, required, lens_seat, nlens,
                                                      pkt->artifact_hash, pkt->workdir, out);
      for (int i = 0; i < filled; i++)
         aimee_log(LOG_INFO, "wfe-panel", "lens '%s' verdict %s from agent '%s' (%d blocker(s))",
                   out[i].persona,
                   out[i].kind == WFE_V_REQUEST_CHANGES ? "request_changes" : "approve",
                   out[i].model, out[i].high_sev_blockers);
      delegate_roundtable_result_free(&rt);
      rc_final = filled > 0 ? filled : 0;
      break;
   }

   free(task);
   return rc_final;
}

void wfe_live_panel_register(void)
{
   wfe_set_panel_provider(live_panel);
   aimee_log(LOG_INFO, "wfe-panel", "live roundtable panel provider registered");
}
