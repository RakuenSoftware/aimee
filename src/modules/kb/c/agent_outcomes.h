/* db1/agent_outcomes.h: per-delegation agent outcome audit log — KB_STORE.
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
#ifndef DEC_KB_STORE_AGENT_OUTCOMES_H
#define DEC_KB_STORE_AGENT_OUTCOMES_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* outcome_kind: matches the outcome_str table in agent_runtime
    * ("success", "partial", "failure", "error"). */
   int kb_store_agent_outcome_record(const char *agent_name, const char *role,
                                     const char *outcome_kind, const char *reason, int turns_used,
                                     int tools_called, int64_t tokens_used,
                                     const char *tool_error_pattern);

   /* (role, reason) pair from a recently-failed/errored row. */
   typedef struct
   {
      char role[64];
      char reason[256];
   } kb_store_agent_outcome_failure_t;

   /* repeated tool_error_pattern with its observed count. */
   typedef struct
   {
      char pattern[256];
      int count;
   } kb_store_agent_outcome_pattern_count_t;

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_AGENT_OUTCOMES_H */
