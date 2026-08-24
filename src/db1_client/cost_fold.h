/* db1/cost_fold.h: idempotent parent<->child session-cost fold log.
 *
 * Each row records that a child session's cost has been folded into a
 * parent's running total. The (parent_session_id, child_session_id)
 * pair is UNIQUE so a re-fold is a no-op — the failure-recovery path
 * for nested orchestrator chains in
 * docs/proposals/pending/delegate-reliability-heartbeat-and-cost-rollup.md
 * Phase 3 can replay folds without double-counting.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_COST_FOLD_H
#define DEC_DB1_COST_FOLD_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Record that `cost_usd` from the child session has been folded into
    * the parent. Returns 1 on first-time insert, 0 if already folded
    * (UNIQUE constraint hit), -1 on error. `source` is a short label
    * such as "model" or "subagent" written verbatim into cost_source. */
   int db1_cost_fold_record(const char *parent_session_id, const char *child_session_id,
                            double cost_usd, const char *source);

   /* Sum of cost_usd across every row where parent_session_id matches.
    * Returns 0.0 for unknown parents or DB-not-initialized. */
   double db1_cost_fold_total(const char *parent_session_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_COST_FOLD_H */
