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

   /* --- delegation_spawns --- */

   /* Record a new spawn with status='running'. Returns 0 on success. */
   int db1_delegation_spawn_record(const char *delegation_id, const char *parent_delegation_id,
                                   const char *session_id, int depth, const char *role);

   /* Mark a spawn as status='done'. Returns 0 on success. */
   int db1_delegation_spawn_complete(const char *delegation_id);

   /* If the spawn has been externally stopped, optionally write "cancelled" or
    * "preempted" to `out` and return 1. Returns 0 for active/running/done/
    * missing rows, -1 on invalid input or DB error. */
   int db1_delegation_spawn_stop_reason(const char *delegation_id, char *out, size_t out_sz);

   /* Returns 1 if the spawn row has status='cancelled' or 'preempted'. Returns
    * 0 for all other statuses and missing rows. */
   int db1_delegation_spawn_is_stopped(const char *delegation_id);

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

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_DELEGATIONS_H */
