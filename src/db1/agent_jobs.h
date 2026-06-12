/* db1/agent_jobs.h: durable per-machine agent-job queue.
 *
 * Each row is one resumable agent invocation with its role/prompt,
 * owning agent name, lease metadata, and result cursor. Used by the
 * server's durable-job path (aimee agent job resume) and the cancel
 * command. Pure per-machine local state.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_AGENT_JOBS_H
#define DEC_DB1_AGENT_JOBS_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_AJ_ROLE_LEN   32
#define DB1_AJ_AGENT_LEN  64
#define DB1_AJ_STATUS_LEN 32
#define DB1_AJ_TS_LEN     32

#define DB1_AJ_TOOL_LEN 64

   /* Ceiling on a stored result, enforced at the write chokepoint
    * (db1_agent_job_update). prompt/result are otherwise unbounded heap; this
    * truncates-with-marker so a single runaway delegate cannot store an
    * arbitrarily large blob. Inbound prompt size is bounded separately by the
    * request-body cap. */
#define DB1_AJ_RESULT_STORE_MAX (1u << 20) /* 1 MiB */

   typedef struct
   {
      int id;
      char role[DB1_AJ_ROLE_LEN];
      /* prompt/result are heap-owned, never NULL after a successful
       * db1_agent_job_get/list_recent; free with db1_agent_job_free. */
      char *prompt;
      char agent_name[DB1_AJ_AGENT_LEN];
      char status[DB1_AJ_STATUS_LEN];
      char *result;
      int cursor_turn;
      char lease_owner[32];
      char heartbeat_at[DB1_AJ_TS_LEN];
      /* current_tool: name of the tool currently in flight, or "" if the
       * delegate is between tool calls (idle / awaiting model). Updated by
       * db1_agent_job_heartbeat_ext. Used by stale-state detection to
       * distinguish in-tool stalls (legitimate long bash) from idle stalls
       * (model wedged with no API call). */
      char current_tool[DB1_AJ_TOOL_LEN];
      /* api_call_count: monotonic count of provider API calls made. A heartbeat
       * with the same (current_tool, api_call_count) across polls means
       * forward progress has stalled. */
      int api_call_count;
      char created_at[DB1_AJ_TS_LEN];
      char updated_at[DB1_AJ_TS_LEN];
   } db1_agent_job_t;

   /* Insert a new job in status='pending'. The worker moves it to
    * status='running' with db1_agent_job_take_lease once it actually starts.
    * Returns the new job id (>0) or -1 on error. */
   int db1_agent_job_create(const char *role, const char *prompt, const char *agent_name,
                            const char *lease_owner);

   /* UPDATE status/cursor_turn/result + updated_at=now. */
   void db1_agent_job_update(int job_id, const char *status, int cursor_turn, const char *result);

   /* UPDATE agent_name + updated_at=now once routing has selected an agent. */
   void db1_agent_job_set_agent(int job_id, const char *agent_name);

   /* UPDATE heartbeat_at/updated_at=now. */
   void db1_agent_job_heartbeat(int job_id);

   /* UPDATE heartbeat_at/updated_at=now AND current_tool, api_call_count.
    * current_tool may be "" or NULL when the delegate is between tool
    * calls (waiting on the model). api_call_count is a monotonic counter
    * of provider API calls made by this delegate; pass the current value.
    * Used by the runtime to write per-tool-call heartbeats so stale-state
    * detection can distinguish idle stalls from in-tool stalls. */
   void db1_agent_job_heartbeat_ext(int job_id, const char *current_tool, int api_call_count);

   /* Returns 1 if the job's status is 'cancelled', 0 otherwise (including
    * if the job does not exist). Cheap status-only read used by the agent
    * loop to honor `aimee cancel job <id>` cooperatively at turn
    * boundaries — it does not pull the full row. */
   int db1_agent_job_is_cancelled(int job_id);

   /* Classify a job's stale state based on heartbeat_at + current_tool.
    * Model waits ("model") use the idle threshold because no external tool is
    * legitimately still running. Forced final-response waits use a built-in
    * minimum so slow final responses are not cut off by aggressive idle
    * thresholds. Review delegates use a shorter in-tool cap because they
    * should inspect and report rather than hold the queue in a long bash.
    *   in_tool_threshold_secs: threshold for tool-dispatch stalls
    *     (default 1200 = 20 min — a legitimate long bash).
    *   idle_threshold_secs:    threshold for idle/model-wait stalls
    *     (default  450 = 7.5 min — a model that hasn't called a tool).
    * Returns one of:
    *   "fresh"     — heartbeat newer than threshold
    *   "in_tool"   — current_tool != "" and heartbeat older than in_tool_threshold
    *   "model"     — model wait older than idle_threshold
    *   "final_response" — final text-only wait older than its capped threshold
    *   "idle"      — current_tool == "" and heartbeat older than idle_threshold
    * out_state must be at least 16 bytes. Returns 1 if stale (in_tool or
    * idle), 0 if fresh. */
   int db1_agent_job_classify_stale(int job_id, int idle_threshold_secs, int in_tool_threshold_secs,
                                    char *out_state, size_t out_state_cap);

   /* Load by id. Returns 0 on hit, -1 on miss. On hit, out->prompt and
    * out->result are heap-allocated (never NULL) and must be released with
    * db1_agent_job_free. On miss, out is zero-initialized (free is still safe). */
   int db1_agent_job_get(int job_id, db1_agent_job_t *out);

   /* Release the heap-owned fields (prompt/result) of a job loaded by
    * db1_agent_job_get / db1_agent_job_list_recent. Idempotent and NULL-safe:
    * tolerates a zero-initialized struct (a failed get) and double calls. */
   void db1_agent_job_free(db1_agent_job_t *job);

   /* Repair finished job rows created before routed agent metadata was
    * persisted. Matches blank agent_name rows to nearby agent_log rows with
    * the same role. Returns rows changed, or -1 on DB error. */
   int db1_agent_job_backfill_agent_names_from_log(void);

   /* Check if a heartbeat_at timestamp is older than `stale_minutes`
    * ago. Returns 1 if stale, 0 if fresh or unparseable. */
   int db1_agent_job_heartbeat_is_stale(const char *heartbeat_at, int stale_minutes);

   /* UPDATE status='running', lease_owner=<owner>, heartbeat_at=now.
    * Returns 0 on success, -1 on error. */
   int db1_agent_job_take_lease(int job_id, const char *owner);

   /* List most recent `max` jobs (ORDER BY id DESC). Returns count. Each
    * returned row owns heap prompt/result; free every returned row with
    * db1_agent_job_free. When include_heavy == 0, the (potentially large)
    * prompt/result are returned as empty strings (not loaded) so list callers
    * that do not serialize them avoid up to max × the result ceiling of
    * allocation; pass 1 only when the bodies are actually needed. */
   int db1_agent_job_list_recent(db1_agent_job_t *out, int max, int include_heavy);

   /* Ids of jobs currently in status='running', up to `max`. */
   int db1_agent_job_list_running_ids(int *out_ids, int max);

   /* Cancel one job by id with a reason. Returns rows changed (0/1). */
   int db1_agent_job_cancel_by_id(int job_id, const char *reason);

   /* Cancel all 'running' jobs older than `threshold_seconds`, with a
    * fixed reason. Returns rows changed. */
   int db1_agent_job_cancel_stale(int threshold_seconds, const char *reason);

   /* One row from the agent_log table. */
   typedef struct
   {
      char agent_name[64];
      char role[32];
      int turns;
      int tool_calls;
      int success;
      int confidence;
      int prompt_tokens;
      int completion_tokens;
      int latency_ms;
      char created_at[32];
   } db1_agent_log_entry_t;

   /* List the most recent `max` rows from agent_log, optionally filtered by
    * agent name (pass NULL to list all agents).  Returns the count written. */
   int db1_agent_log_list(const char *agent_filter, db1_agent_log_entry_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_AGENT_JOBS_H */
