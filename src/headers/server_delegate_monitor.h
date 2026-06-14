/* server_delegate_monitor.h: aimee-server background thread that polls
 * running agent_jobs and auto-cancels delegates whose
 * (current_tool, api_call_count, heartbeat_at) row indicates a stall.
 *
 * Pairs with PR #1583 (heartbeat substrate) and PR #1584 (cooperative
 * cancellation). The monitor itself does not interrupt the running
 * delegate — it flips agent_jobs.status to 'cancelled', and the agent
 * runtime's per-turn db1_agent_job_is_cancelled check halts the loop at
 * the next turn boundary.
 *
 * Enabled by default. Set AIMEE_DELEGATE_HEARTBEAT_MONITOR=0 to disable
 * the monitor while debugging delegate runtime behavior. */
#ifndef DEC_SERVER_DELEGATE_MONITOR_H
#define DEC_SERVER_DELEGATE_MONITOR_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Idempotent. If AIMEE_DELEGATE_HEARTBEAT_MONITOR=0, no thread starts
    * and this is a no-op. Safe to call from server_init unconditionally. */
   void server_delegate_monitor_init(void);

   /* Idempotent. Joins the monitor thread (if started). Safe to call from
    * server_shutdown unconditionally. */
   void server_delegate_monitor_shutdown(void);

   /* Test seam: classify every running agent_job and cancel the stale
    * ones. Returns the number cancelled. Called by the monitor thread
    * once per poll interval; exposed so tests can drive the same logic
    * without spawning the thread. */
   int server_delegate_monitor_sweep(int idle_threshold_secs, int in_tool_threshold_secs);

   /* Bind/unbind a per-turn heartbeat for the in-flight background delegate on
    * THIS thread. begin() registers an http_retry progress callback that bumps
    * agent_jobs.heartbeat_at after every model HTTP attempt, so a slow-but-
    * progressing delegate (each turn bounded by the agent timeout, under the
    * stale idle threshold) is not auto-cancelled as stalled — the reason only the
    * fastest model used to survive the fleet. job_id <= 0 is a no-op. Always pair
    * begin() with end(). */
   void server_delegate_heartbeat_begin(int job_id);
   void server_delegate_heartbeat_end(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_SERVER_DELEGATE_MONITOR_H */
