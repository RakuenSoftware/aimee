/* wfe_live_panel.c -- the live roundtable panel provider.
 *
 * gate.roundtable calls this to convene a diverse panel THROUGH THE ROUNDTABLE
 * ENGINE (delegate_roundtable_run, REVIEW mode): one structured-review panelist
 * per REQUIRED persona, whose findings are captured as review items with
 * replayable evidence, deduped across the panel, replay-VERIFIED against the
 * gate's worktree (wfe_replay_worktree — interpretation never blocks, a
 * contradicted claim is rejected), and finally mapped onto per-lens verdicts
 * (wfe_panel_verdicts_from_roundtable) for the fail-closed wfe_gate_decide.
 * Registered from wfe_autonomy_register.
 *
 * Seat semantics are unchanged from the pre-engine panel: a PINNED seat model
 * is never substituted (unfulfillable -> the run FAILS); a "$random" or
 * unmatched lens picks any review-capable agent, preferring panel diversity but
 * reusing a seated agent when the roster is smaller than the panel; and when NO
 * review agent is eligible right now the panel QUEUES for a seat up to
 * AIMEE_PANEL_SEAT_WAIT_SECS before the gate degrades.
 *
 * NOTE: panelists are tool-less (the engine embeds the change in the prompt),
 * so the panel no longer touches the worktree at all; only the verification
 * pass reads it. Dispatch requires reachable review agents, so this provider is
 * exercised by integration (a live deployment); the risk-bearing pieces — the
 * verdict mapping and the worktree replay backend — are unit-tested in
 * test_wfe_panel_roundtable / test_wfe_replay_worktree. */
#include "aimee.h"

#include "wfe_panel_roundtable.h"
#include "wfe_replay_worktree.h"
#include "wfe_roundtable.h"

#include "agent_config.h"
#include "config.h"
#include "delegate_ensemble.h"
#include "log.h"
#include "roundtable_preset.h"
#include "roundtable_seat_resolve.h"
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

/* The engine review task: what was asked plus the change under review. The
 * panelists are tool-less — the diff IS the material — and the engine's own
 * REVIEW round instruction supplies the structured-items output contract
 * (severity/category/location/summary/recommendation + replayable evidence). */
static char *build_review_task(const wfe_review_packet_t *pkt)
{
   const char *focus =
       (pkt->focus && pkt->focus[0]) ? pkt->focus : "correctness, quality, and completeness";
   const char *proposal = (pkt->proposal && pkt->proposal[0]) ? pkt->proposal : "(none provided)";
   const char *diff = (pkt->diff && pkt->diff[0])
                          ? pkt->diff
                          : "(no code diff — review the plan/proposal artifact against the ask)";
   size_t cap = 1024 + strlen(focus) + strlen(proposal) + strlen(diff);
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   snprintf(buf, cap,
            "Review the CHANGE UNDER REVIEW below AGAINST what was asked.\n\n"
            "FOCUS: %s\n\nORIGINAL PROPOSAL/REQUEST:\n%.4000s\n\n"
            "CHANGE UNDER REVIEW (diff vs the base repo):\n%s\n\n"
            "For every item, location is \"file:line\" from the change wherever possible, and a "
            "blocking severity REQUIRES reproducible factual evidence about this code.",
            focus, proposal, diff);
   return buf;
}

/* The seat model bound to `persona` in the active roundtable preset, or NULL when
 * no seat matches (the caller then treats the lens as "$random"). The model may
 * itself be the "$random" sentinel — a seat the user explicitly set to random. */
static const char *seat_model_for_persona(const roundtable_preset_t *preset, const char *persona)
{
   if (!preset || !persona || !persona[0])
      return NULL;
   for (int i = 0; i < preset->seat_count; i++)
      if (strcmp(preset->seats[i].persona, persona) == 0)
         return preset->seats[i].model;
   return NULL;
}

/* Load the roundtable preset this gate convenes into *preset for its
 * persona->model bindings. `requested` is the node's params.roundtable (the gate
 * may name a specific preset); when empty it falls back to the configured default
 * (roundtable.default, else "default"). Returns 1 on success, 0 when no preset
 * loads — in which case every lens resolves as "$random", preserving the
 * pre-preset "any review-capable agent per persona" behaviour. A NAMED preset
 * that does not load is logged, since it is likely a workflow authoring error. */
static int load_panel_preset(roundtable_preset_t *preset, const char *requested)
{
   memset(preset, 0, sizeof *preset);
   config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   const char *name = "default";
   if (config_load(&cfg) == 0 && cfg.roundtable_default[0])
      name = cfg.roundtable_default;
   if (requested && requested[0])
      name = requested; /* the node's explicit choice wins over the default */
   if (roundtable_preset_load(name, preset) == 0)
      return 1;
   if (requested && requested[0])
      aimee_log(LOG_WARN, "wfe-panel",
                "roundtable preset '%s' not found -> every lens falls back to $random", requested);
   return 0;
}

/* Resolve every lens to a concrete review agent. Pinned seats resolve exactly
 * once (unfulfillable -> WFE_PANEL_PINNED_FAIL); $random seats prefer a UNIQUE
 * agent per lens and downgrade to reuse when the roster is smaller than the
 * panel. Returns 0 with seat[0..nlens-1] set (pointers into acfg), or a
 * WFE_PANEL_* sentinel; `*any_pinned` reports whether any seat is pinned.
 * Returns 1 when no review agent is eligible AT ALL right now (caller queues). */
static int resolve_seats(agent_config_t *acfg, const roundtable_preset_t *preset, int have_preset,
                         const char *const *required, int nlens, const char *seat[],
                         int *any_pinned)
{
   const char *used[MAX_AGENTS];
   int nused = 0;
   *any_pinned = 0;
   for (int i = 0; i < nlens; i++)
      seat[i] = NULL;

   for (int i = 0; i < nlens; i++)
   {
      const char *model = have_preset ? seat_model_for_persona(preset, required[i]) : NULL;
      if (rt_seat_is_random(model))
         continue; /* $random / unmatched -> second pass */
      int idx = -1;
      if (rt_resolve_seat_model(acfg, model, "review", used, nused, &idx) != RT_SEAT_OK)
      {
         aimee_log(LOG_WARN, "wfe-panel", "pinned model '%s' for lens '%s' unavailable -> fail run",
                   model, required[i]);
         return WFE_PANEL_PINNED_FAIL;
      }
      seat[i] = acfg->agents[idx].name;
      *any_pinned = 1;
      if (nused < MAX_AGENTS)
         used[nused++] = seat[i];
   }

   for (int i = 0; i < nlens; i++)
   {
      if (seat[i])
         continue;
      int idx = -1, reused = 0;
      rt_seat_resolve_t rc =
          rt_resolve_seat_model(acfg, RT_SEAT_RANDOM, "review", used, nused, &idx);
      if (rc == RT_SEAT_RANDOM_EXHAUSTED)
      {
         rc = rt_resolve_seat_model(acfg, RT_SEAT_RANDOM, "review", NULL, 0, &idx);
         reused = 1;
      }
      if (rc != RT_SEAT_OK || idx < 0 || idx >= acfg->agent_count)
         return 1; /* no eligible review agent at all right now -> queue */
      seat[i] = acfg->agents[idx].name;
      if (reused)
         aimee_log(LOG_INFO, "wfe-panel",
                   "reusing agent '%s' for lens '%s' (fewer eligible review agents than lenses)",
                   seat[i], required[i]);
      else if (nused < MAX_AGENTS)
         used[nused++] = seat[i];
   }
   return 0;
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

   roundtable_preset_t preset;
   int have_preset = load_panel_preset(&preset, pkt->roundtable);

   config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   if (config_load(&cfg) != 0)
      return WFE_PANEL_UNREACHABLE;

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

      const char *seat[WFE_PANEL_MAX];
      int any_pinned = 0;
      int src = resolve_seats(&acfg, &preset, have_preset, required, nlens, seat, &any_pinned);
      if (src == WFE_PANEL_PINNED_FAIL)
      {
         rc_final = WFE_PANEL_PINNED_FAIL;
         break;
      }
      if (src == 1)
      {
         /* Nothing seatable right now: queue for a seat until the deadline. */
         if (final)
         {
            aimee_log(LOG_WARN, "wfe-panel", "no viable review agent for the panel -> degrade");
            rc_final = 0;
            break;
         }
         if (!queued_logged)
         {
            aimee_log(LOG_INFO, "wfe-panel",
                      "no eligible review agent for %d lens(es); queueing up to %lds", nlens,
                      seat_wait);
            queued_logged = 1;
         }
         struct timespec nap = {WFE_PANEL_SEAT_POLL_SECS, 0};
         nanosleep(&nap, NULL);
         continue;
      }

      /* Panel composition = the resolved seats; each lens rides in as the
       * panelist's persona. The engine's internal index-backed replay stays
       * OFF: the gate verifies against the WORKTREE below instead (the index
       * lags the change under review). All lenses are required, so anything
       * short of a full panel is a failed attempt. */
      cfg.ensemble_reference_count = nlens;
      cfg.ensemble_reference_persona_count = nlens;
      for (int i = 0; i < nlens; i++)
      {
         snprintf(cfg.ensemble_reference_models[i], sizeof cfg.ensemble_reference_models[i], "%s",
                  seat[i]);
         snprintf(cfg.ensemble_reference_personas[i], sizeof cfg.ensemble_reference_personas[i],
                  "%s", required[i]);
      }
      cfg.ensemble_min_successful = nlens;
      cfg.roundtable_replay_verify_enabled = 0;

      roundtable_opts_t opts;
      memset(&opts, 0, sizeof opts);
      opts.mode = ROUNDTABLE_REVIEW;
      opts.turns = ROUNDTABLE_PARALLEL;
      opts.max_rounds = 1;
      opts.deadline_ms = WFE_PANEL_DEADLINE_MS;

      roundtable_result_t rt;
      memset(&rt, 0, sizeof rt);
      if (delegate_roundtable_run(&acfg, &cfg, task, &opts, &rt) != 0)
      {
         delegate_roundtable_result_free(&rt);
         rc_final = WFE_PANEL_UNREACHABLE;
         break;
      }

      if (rt.participants_failed > 0 || rt.degraded)
      {
         aimee_log(LOG_WARN, "wfe-panel", "panel attempt: %d/%d panelist(s) failed%s%s",
                   rt.participants_failed, rt.participants_total, rt.degraded ? " (degraded)" : "",
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
      roundtable_verify_items_with(&rt, wfe_replay_worktree_backend());
      wfe_replay_worktree_set_root(NULL);
      aimee_log(LOG_INFO, "wfe-panel",
                "panel items: %d kept (verified=%d capped=%d degraded=%d), %d rejected",
                rt.item_count, rt.verified_count, rt.capped_count, rt.degraded_count,
                rt.rejected_count);
      for (int i = 0; i < rt.rejected_count && i < ROUNDTABLE_MAX_REVIEW_ITEMS; i++)
         aimee_log(LOG_WARN, "wfe-panel", "rejected finding (%s) [%s] %s: %s",
                   rt.rejected_reason[i], rt.rejected[i].sources, rt.rejected[i].location,
                   rt.rejected[i].summary);

      int filled = wfe_panel_verdicts_from_roundtable(&rt, required, seat, nlens,
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
