/* roundtable_pipeline_eval.c: pure decision logic for the authoring pipeline
 * outer loop. See roundtable_pipeline_eval.h and the proposal's section 3. */

#include "roundtable_pipeline_eval.h"
#include "roundtable_pipeline.h"

#include <string.h>

int roundtable_terminal_envelope_valid(const rtp_envelope_t *e)
{
   if (!e)
      return 0;
   /* capture / parse integrity */
   if (!e->present || !e->parse_ok || e->has_error || e->lost_result)
      return 0;
   /* engine terminal flags that make the result unusable evidence (#13/#38) */
   if (e->truncated || e->items_truncated || e->degraded || e->cost_capped || e->deadline_hit ||
       e->cancelled)
      return 0;
   /* draft passes carry an artifact, not items; a draft with no artifact is
    * unusable. */
   if (e->is_draft)
      return e->artifact_present ? 1 : 0;
   /* review provenance: the surfaced artifact_round must match the evaluated
    * items_round when both are present (#41). A mismatch means the items and the
    * reviewed artifact came from different rounds — ambiguous evidence. */
   if (e->items_round > 0 && e->artifact_round > 0 && e->items_round != e->artifact_round)
      return 0;
   return 1;
}

static int sev_is(const char *bar, const char *want)
{
   return bar && want && strcmp(bar, want) == 0;
}

rtp_donebar_result_t rtp_donebar_eval(const char *done_bar, const rtp_envelope_t *e,
                                      int accepted_question_count)
{
   if (!roundtable_terminal_envelope_valid(e))
      return RTP_DONEBAR_INVALID;
   /* A review done-bar requires the engine's deterministic convergence. */
   if (!e->converged)
      return RTP_DONEBAR_BLOCKED;
   /* zero_blocking (default): no blocking items. */
   if (e->blocking_count > 0)
      return RTP_DONEBAR_BLOCKED;

   if (sev_is(done_bar, RTP_DONEBAR_ZERO_BLOCKING_SUGGESTIONS))
   {
      if (e->suggestion_count > 0)
         return RTP_DONEBAR_BLOCKED;
   }
   else if (sev_is(done_bar, RTP_DONEBAR_ZERO_BLOCKING_QUESTIONS))
   {
      /* The brief must carry at most RTP_MAX_BRIEF_QUESTIONS; overflow is not
       * treated as answered (#14). */
      if (accepted_question_count > RTP_MAX_BRIEF_QUESTIONS)
         return RTP_DONEBAR_INVALID;
      /* every accepted question answered and no coverage gaps. */
      if (e->answered_count < accepted_question_count || e->coverage_gap_count > 0)
         return RTP_DONEBAR_BLOCKED;
   }
   /* default zero_blocking and any unknown bar fall through as met once
    * converged + no blocking. */
   return RTP_DONEBAR_MET;
}

int rtp_chunk_aggregate_done(int chunk_total, int chunk_done, int synthesis_done,
                             int any_chunk_invalid)
{
   if (chunk_total <= 0)
      return 0;
   if (any_chunk_invalid)
      return 0;
   if (chunk_done < chunk_total)
      return 0;
   return synthesis_done ? 1 : 0;
}

/* An infra fault is a capture/transport failure (not a quality verdict): the
 * pass can be re-run under the same pass id with a fresh attempt (#19/#29).
 * Engine-quality invalidity (degraded/truncated/cost_capped/deadline_hit/
 * cancelled) is NOT an infra fault — it escalates rather than silently looping. */
static int envelope_is_infra_fault(const rtp_envelope_t *e)
{
   if (!e)
      return 1;
   return (!e->present || !e->parse_ok || e->has_error || e->lost_result);
}

rtp_action_t rtp_loop_decide(const rtp_loop_cfg_t *cfg, const rtp_loop_state_t *st,
                             const rtp_envelope_t *e)
{
   int max_attempts = (cfg && cfg->max_attempts_per_pass > 0) ? cfg->max_attempts_per_pass : 2;

   if (!roundtable_terminal_envelope_valid(e))
   {
      if (envelope_is_infra_fault(e))
      {
         /* re-run the same pass id with a new attempt, bounded by the retry
          * ceiling; otherwise escalate (never silently spin, #29). */
         if (st && st->attempt_no < max_attempts)
            return RTP_ACT_RETRY;
         return RTP_ACT_ESCALATE;
      }
      /* captured-but-invalid engine evidence -> human (#38/#13). */
      return RTP_ACT_ESCALATE;
   }

   rtp_donebar_result_t db = rtp_donebar_eval(cfg ? cfg->done_bar : RTP_DONEBAR_ZERO_BLOCKING, e,
                                              st ? st->accepted_question_count : 0);
   if (db == RTP_DONEBAR_MET)
      return RTP_ACT_PASS;
   if (db == RTP_DONEBAR_INVALID)
      return RTP_ACT_ESCALATE;

   /* blocked: another revise pass is needed. Cost backstops escalate to the
    * human rather than auto-passing a not-done artifact (proposal section 3). */
   if (cfg && cfg->max_passes > 0 && st && st->pass_no >= cfg->max_passes)
      return RTP_ACT_ESCALATE;
   if (cfg && cfg->max_phase_cost_usd > 0.0 && st && st->phase_cost_usd >= cfg->max_phase_cost_usd)
      return RTP_ACT_ESCALATE;
   return RTP_ACT_REVISE;
}

int rtp_gate_authority_ok(const char *provided_principal, const char *active_operator_id,
                          int operator_active)
{
   if (!operator_active)
      return 0; /* no enrolled operator -> nobody can resolve a gate yet */
   if (!provided_principal || !provided_principal[0])
      return 0; /* a delegate-driving session presents no operator principal */
   if (!active_operator_id || !active_operator_id[0])
      return 0;
   return strcmp(provided_principal, active_operator_id) == 0 ? 1 : 0;
}
