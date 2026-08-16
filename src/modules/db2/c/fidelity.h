#ifndef AIMEE_DB2_FIDELITY_H
#define AIMEE_DB2_FIDELITY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* auditable-correctness P3 — fidelity storage substrate.
    *
    * Answer-level fidelity is recorded as a NON-SCORED 'fidelity_report' artifact
    * keyed by turn_id, and per-chunk verdicts as 'fidelity_attribution' artifacts
    * (operator_id 'fidelity-judge'). Both kinds are structurally invisible to
    * db2_demotion_score, which reads only kind='retrieval_attribution' — so
    * fidelity feeds NOTHING into demotion (the proposal's "demotion-inert by
    * construction" guarantee). The LLM entailment judge that PRODUCES these rows
    * is a later, default-off increment; this layer is the storage + read only. */

   /* Write an answer-level fidelity report for `turn_id`. `status` is one of the
    * four exhaustive audit states ("ok" | "not_evaluated" | "evidence_unavailable"
    * | "not_instrumented"); supported/unsupported/abstained are claim-bucket counts
    * (abstained = judge below the confidence floor, kept distinct so an uncertain
    * judge never inflates the supported rate). `turn_id` is required and `status`
    * must be one of the four states above (both rejected with -1 otherwise). Writing
    * a report is the (RE)JUDGE BOUNDARY for the turn: it replaces the prior report
    * AND deletes the turn's prior per-chunk attributions, so the caller then writes
    * fresh attributions and no stale ones survive. Returns 0 / -1. */
   int db2_fidelity_report_write(const char *turn_id, const char *status, int supported,
                                 int unsupported, int abstained);

   /* Read the fidelity report for `turn_id` (exactly one exists per turn). Any
    * out-param may be NULL. Returns 1 (found), 0 (none), -1 (error — incl. a present
    * row whose payload is malformed, distinct from a legitimate all-zeros report). */
   int db2_fidelity_report_by_turn(const char *turn_id, char *status_out, int status_out_len,
                                   int *supported_out, int *unsupported_out, int *abstained_out);

   /* Write a per-chunk fidelity attribution for `turn_id` (required): `surfaced_id`
    * is the memory/source id the judge assessed, `verdict` is "accepted" |
    * "irrelevant" (rejected with -1 otherwise). Stored as kind='fidelity_attribution',
    * operator_id='fidelity-judge' — never a retrieval_attribution, so it cannot shift
    * any demotion percentile. Call AFTER db2_fidelity_report_write for the turn (that
    * write clears prior attributions). 0 / -1. */
   int db2_fidelity_attribution_write(const char *turn_id, int64_t surfaced_id,
                                      const char *verdict);

   /* Count the per-chunk fidelity attributions recorded for `turn_id`. Returns the
    * count (>=0) or -1 on error. (A full verdict-list reader arrives with the audit
    * surface; this is the substrate's attribution read path.) */
   int db2_fidelity_attribution_count_by_turn(const char *turn_id);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_FIDELITY_H */
