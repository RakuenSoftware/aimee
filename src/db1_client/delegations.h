/* db1/delegations.h: user-local delegation conversation channel state.
 *
 * Two tables:
 *   - delegation_spawns: tracks every spawned delegation so the runtime
 *     can enforce per-session spawn budgets and propagate cancellation.
 *   - delegation_messages: turn-by-turn log of delegate↔aimee messages.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_DELEGATIONS_H
#define DEC_DB1_DELEGATIONS_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* --- delegation_messages --- */

   /* Insert a single message. Returns 0 on success, -1 on error. */
   int db1_delegation_message_record(const char *delegation_id, const char *direction,
                                     const char *content);

   /* --- delegation_spawns --- */

   /* Record a new spawn with status='running'. Returns 0 on success. */
   int db1_delegation_spawn_record(const char *delegation_id, const char *parent_delegation_id,
                                   const char *session_id, int depth, const char *role);

   /* Mark a spawn as status='done'. Returns 0 on success. */
   int db1_delegation_spawn_complete(const char *delegation_id);

   /* Mark a running/active spawn as status='preempted'. Returns 1 if a row was
    * updated, 0 if no matching active row, -1 on error. */
   int db1_delegation_spawn_preempt(const char *delegation_id);

   /* Read the current spawn status into `out`. Returns 0 on success, -1 on
    * invalid input, missing row, or DB error. */
   int db1_delegation_spawn_status(const char *delegation_id, char *out, size_t out_sz);

   /* If the spawn has been externally stopped, optionally write "cancelled" or
    * "preempted" to `out` and return 1. Returns 0 for active/running/done/
    * missing rows, -1 on invalid input or DB error. */
   int db1_delegation_spawn_stop_reason(const char *delegation_id, char *out, size_t out_sz);

   /* Returns 1 if the spawn row has status='cancelled' or 'preempted'. Returns
    * 0 for all other statuses and missing rows. */
   int db1_delegation_spawn_is_stopped(const char *delegation_id);

   /* Returns 1 if the spawn row for `delegation_id` has status='cancelled'.
    * Returns 0 if not cancelled or the row doesn't exist. */
   int db1_delegation_spawn_is_cancelled(const char *delegation_id);

   /* Returns 1 if the spawn row for `delegation_id` is currently active/running.
    * Returns 0 for terminal, stale, missing, or invalid rows. */
   int db1_delegation_spawn_is_active(const char *delegation_id);

   /* Count every spawn row ever recorded for `session_id`, regardless of
    * status. Used for diagnostics and legacy budget checks. */
   int db1_delegation_spawn_count_total(const char *session_id);

   /* Find the top-level delegate ancestor for `delegation_id`. If the row has
    * no parent, the row itself is the root. Returns 0 on success, -1 on error
    * or missing row. */
   int db1_delegation_spawn_find_root(const char *delegation_id, char *out, size_t out_sz);

   /* Count every descendant of a top-level delegate, regardless of status.
    * The root delegate itself is not counted. Used to cap nested delegate
    * fan-out without exhausting long operator sessions that launch many
    * first-level delegates sequentially. */
   int db1_delegation_spawn_count_descendants(const char *root_delegation_id);

   /* Write the integer ids of spawns in ('active', 'running') into
    * `out_ids` (up to `max` ids). Returns the number of ids written, or
    * -1 on error. */
   int db1_delegation_spawn_list_active(int *out_ids, int max);

   /* Cancel a specific spawn row by integer id. Returns 1 if a row was
    * updated, 0 if no matching active row, -1 on error. */
   int db1_delegation_spawn_cancel_by_id(int spawn_id);

   /* Cancel `spawn_id` and every active/running descendant linked through
    * parent_delegation_id. Walks the tree via a recursive CTE and applies
    * one UPDATE so the cascade is atomic.
    *
    * Used by `aimee cancel delegation <id>` so cancelling an orchestrator
    * also halts every worker it spawned (and recursively their workers).
    * Without this, a cancelled parent leaves children running until they
    * themselves notice the parent is gone, wasting tokens and burning the
    * spawn budget for the rest of the session.
    *
    * Returns the total number of rows cancelled (>= 0 on success), -1 on
    * error. */
   int db1_delegation_spawn_cancel_recursive(int spawn_id);

   /* Cancel all spawns that have been 'active' or 'running' for longer than
    * 24 hours (orphan cleanup). Returns the number of rows cancelled. */
   int db1_delegation_spawn_cancel_stale(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_DELEGATIONS_H */
