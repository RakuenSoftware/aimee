/* db1/agent_outcomes.h: per-delegation agent outcome audit log — DB2.
 *
 * Records the outcome of each delegated agent run: success/failure/error,
 * the role and reason, token + tool counts, and an optional tool-error
 * pattern for the auto-anti-pattern miner. Cross-session audit data feeding the anti-pattern miner.
 *
 * Read paths used by the eval feedback loop:
 *   - distinct (role, reason) for recent failures (window in days)
 *   - repeated tool_error_pattern (count >= threshold)
 *
 * Write path used by the agent runtime: record one row per delegation. */
#ifndef DEC_DB2_AGENT_OUTCOMES_H
#define DEC_DB2_AGENT_OUTCOMES_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* outcome_kind: matches the outcome_str table in agent_runtime
    * ("success", "partial", "failure", "error"). */
   int db2_agent_outcome_record(const char *agent_name, const char *role, const char *outcome_kind,
                                const char *reason, int turns_used, int tools_called,
                                int64_t tokens_used, const char *tool_error_pattern);

   /* (role, reason) pair from a recently-failed/errored row. */
   typedef struct
   {
      char role[64];
      char reason[256];
   } db2_agent_outcome_failure_t;

   /* Distinct (role, reason) over the last `window_days` rows whose outcome
    * is failure or error, newest first. Returns count written to out
    * (capped at max). */
   int db2_agent_outcome_recent_failures(int window_days, db2_agent_outcome_failure_t *out,
                                         int max);

   /* repeated tool_error_pattern with its observed count. */
   typedef struct
   {
      char pattern[256];
      int count;
   } db2_agent_outcome_pattern_count_t;

   /* GROUP BY tool_error_pattern HAVING COUNT(*) >= min_count. Returns
    * count written to out (capped at max). */
   int db2_agent_outcome_repeated_error_patterns(int min_count,
                                                 db2_agent_outcome_pattern_count_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_AGENT_OUTCOMES_H */
