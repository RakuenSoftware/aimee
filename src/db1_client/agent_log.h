/* db1/agent_log.h: per-machine audit log of agent/tool turns.
 *
 * One row per agent invocation with token counts, latency, success
 * outcome, confidence, and attached session id. Used by the dashboard
 * (recent delegations + per-role metrics), session history (per-session
 * drill-down + search), memory learning (failure pattern extraction),
 * and the HUD (total counts).
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_AGENT_LOG_H
#define DEC_DB1_AGENT_LOG_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_AL_AGENT_LEN       64
#define DB1_AL_ROLE_LEN        32
#define DB1_AL_ERROR_LEN       256
#define DB1_AL_LEARN_ERROR_LEN 1024
#define DB1_AL_SESSION_LEN     128
#define DB1_AL_TS_LEN          32

   typedef struct
   {
      const char *agent_name;
      const char *role;
      int prompt_tokens;
      int completion_tokens;
      int latency_ms;
      int success;       /* 0/1 */
      const char *error; /* may be NULL */
      int turns;
      int tool_calls;
      int confidence;         /* -1 = unscored */
      const char *session_id; /* may be NULL/"" */
   } db1_agent_log_insert_row_t;

   /* Insert one agent_log row. Returns the new row id (> 0) on success, or -1 on
    * error. The id links the matching token_audit row (agent_log_id) so agent
    * stats join 1:1 instead of by the lossy (agent_name, role) key. */
   long long db1_agent_log_insert(const db1_agent_log_insert_row_t *row);

   /* Read-side row used by list/search helpers. Strings are owned by
    * the struct; sized to common display needs (long input/output
    * columns are not surfaced here — use a dedicated helper if needed). */
   typedef struct
   {
      int64_t id;
      char agent_name[DB1_AL_AGENT_LEN];
      char role[DB1_AL_ROLE_LEN];
      int prompt_tokens;
      int completion_tokens;
      int latency_ms;
      int success;
      int turns;
      int tool_calls;
      int confidence;
      char session_id[DB1_AL_SESSION_LEN];
      char created_at[DB1_AL_TS_LEN];
      char error[DB1_AL_ERROR_LEN];
   } db1_agent_log_display_t;

   /* Most-recent `max` rows ordered by id DESC. */
   int db1_agent_log_list_recent(db1_agent_log_display_t *out, int max);

   /* All rows for `session_id`, oldest-first (ORDER BY id ASC). */
   int db1_agent_log_list_by_session(const char *session_id, db1_agent_log_display_t *out, int max);

   /* Distinct session_ids whose role matches the LIKE `pattern`, newest
    * first. Writes ids into `out_ids` (each slot DB1_AL_SESSION_LEN). */
   int db1_agent_log_search_session_ids_by_role(const char *pattern,
                                                char (*out_ids)[DB1_AL_SESSION_LEN], int max);

   typedef struct
   {
      char role[DB1_AL_ROLE_LEN];
      int count;
   } db1_agent_log_role_count_t;

   /* Per-role counts, optionally filtered by created_at >= since. */
   int db1_agent_log_count_per_role(const char *since_or_null, db1_agent_log_role_count_t *out,
                                    int max);

   typedef struct
   {
      char role[DB1_AL_ROLE_LEN];
      char error[DB1_AL_ERROR_LEN];
      char created_at[DB1_AL_TS_LEN];
   } db1_agent_log_failure_t;

   /* Recent failures with a tighter window (seconds-granularity). Used by
    * agent_runtime's per-turn "recent failures" injection. */
   int db1_agent_log_failures_since_seconds(int max_rows, int since_secs,
                                            db1_agent_log_failure_t *out);

   typedef struct
   {
      char error[DB1_AL_LEARN_ERROR_LEN];
   } db1_agent_log_recent_error_t;

   /* Recent non-empty error strings for DB1->DB2 learning flows. */
   int db1_agent_log_list_recent_errors(int since_days, db1_agent_log_recent_error_t *out, int max);

   typedef struct
   {
      char role[DB1_AL_ROLE_LEN];
      char agent_name[DB1_AL_AGENT_LEN];
      int wins;
      int fails;
      int total;
      int avg_turns;
      int avg_tools;
      char recent_error[DB1_AL_LEARN_ERROR_LEN];
   } db1_agent_log_delegation_pattern_t;

   /* Per-(role, agent) aggregates used by memory promotion. */
   int db1_agent_log_list_delegation_patterns(int since_days, int min_total,
                                              db1_agent_log_delegation_pattern_t *out, int max);

   typedef struct
   {
      char role[DB1_AL_ROLE_LEN];
      char agent_name[DB1_AL_AGENT_LEN];
      int fails;
      char errors[DB1_AL_LEARN_ERROR_LEN];
   } db1_agent_log_failure_episode_seed_t;

   /* Grouped failure clusters used by failure-episode synthesis. */
   int db1_agent_log_list_failure_episode_seeds(int since_days, int min_fails,
                                                db1_agent_log_failure_episode_seed_t *out, int max);

   /* Metrics view that joins agent_log with token_audit (both DB1 now).
    * One row per role, ordered by total DESC. */
   typedef struct
   {
      char role[DB1_AL_ROLE_LEN];
      int total;
      int successes;
      int avg_latency_ms;
      int64_t tokens;
      int64_t cache_write_tokens;
      int64_t cache_read_tokens;
      double estimated_cost_usd;
   } db1_agent_log_metric_t;

   int db1_agent_log_metrics_by_role(db1_agent_log_metric_t *out, int max);

   typedef struct
   {
      char agent_name[DB1_AL_AGENT_LEN];
      int total_calls;
      int total_prompt_tokens;
      int total_completion_tokens;
      int avg_latency_ms;
      double success_rate;
      int64_t total_cache_write_tokens;
      int64_t total_cache_read_tokens;
      double total_estimated_cost_usd;
   } db1_agent_log_agent_stats_t;

   /* Per-agent_name stats across agent_log JOIN token_audit. If
    * `agent_name_or_null` is non-NULL, the filter applies; otherwise
    * returns all agents ordered by total call count DESC. */
   int db1_agent_log_agent_stats(const char *agent_name_or_null, db1_agent_log_agent_stats_t *out,
                                 int max);

   typedef struct
   {
      int total_calls;
      int successful_calls;
      int failed_calls;
      int64_t total_prompt_tokens;
      int64_t total_completion_tokens;
      int total_turns;
      int total_tool_calls;
      double avg_latency_ms;
      int recent_calls;
      int recent_successes;
   } db1_agent_log_hud_t;

   /* Aggregate counts for the HUD. `recent_secs` defines the "recent"
    * window used for recent_calls / recent_successes. */
   int db1_agent_log_hud_summary(db1_agent_log_hud_t *out, int recent_secs);

   /* success/total counts for a specific session. Returns 0 on success. */
   int db1_agent_log_session_outcome(const char *session_id, int *successes_out, int *total_out);

   typedef struct
   {
      char agent_name[DB1_AL_AGENT_LEN];
      char role[DB1_AL_ROLE_LEN];
      int total;
      int successes;
      int prompt_tokens;
      int completion_tokens;
      int avg_latency_ms;
      int tool_calls;
   } db1_agent_log_prometheus_t;

   /* Per-(agent, role) aggregates for prometheus exposition. */
   int db1_agent_log_prometheus(db1_agent_log_prometheus_t *out, int max);

   typedef struct
   {
      int total;
      int64_t turns;
      int64_t tool_calls;
      int64_t prompt_tokens;
      int64_t completion_tokens;
      int successes;
   } db1_agent_log_stats_t;

   /* Aggregate stats across agent_log, optionally filtered by
    * created_at >= since. */
   int db1_agent_log_stats(const char *since_or_null, db1_agent_log_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_AGENT_LOG_H */
