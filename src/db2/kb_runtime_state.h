/* kb_runtime_state.h: DB2-backed runtime state owned by aimee-kb.
 *
 * Holds keys that belong to the aimee-kb process (incl. pgvector): the
 * vector schema version last stamped by a rebuild, and the rebuild-lock
 * heartbeat.
 * These live in DB2 (not DB1) because they describe kb-process state,
 * not per-machine state.
 *
 * Pure domain API.  Callers never see the backing handle.
 */
#ifndef DEC_DB2_KB_RUNTIME_STATE_H
#define DEC_DB2_KB_RUNTIME_STATE_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Insert or replace (state_key, state_value).  0 on success, -1 on error. */
   int db2_kb_runtime_state_set(const char *key, const char *value);

   /* Read value for `key`.  0 on hit, -1 on miss or error.  Writes an empty
    * string when the stored value is empty. */
   int db2_kb_runtime_state_get(const char *key, char *out, size_t out_len);

   /* Delete the row for `key`.  Returns 0 on success. */
   int db2_kb_runtime_state_delete(const char *key);

   /* Upsert `key` = datetime('now'). */
   int db2_kb_runtime_state_set_now(const char *key);

   /* Vector rebuild-lock predicate: returns 1 if a fresh lock row exists. */
   int db2_kb_runtime_state_vector_rebuild_lock_held(void);

   /* Non-atomic try-acquire: if no fresh lock is held, plant a
    * datetime('now') row and return 1.  Otherwise 0. */
   int db2_kb_runtime_state_vector_rebuild_lock_try_acquire(void);

   /* Unconditionally drop the vector rebuild-lock row. */
   void db2_kb_runtime_state_vector_rebuild_lock_release(void);

   /* ── Project-purge generation fence (webchat-project-lifecycle slice 2) ──
    * Fence rows: `project_purging:<key>` = "<generation> <purge_id>" plus a
    * `project_purging_ts:<key>` heartbeat written via pg_now_text(). A fence
    * whose heartbeat is older than kb_purge_fence_ttl_s (default 900) is
    * treated as absent by writers. */

   /* Write (or overwrite) the fence for `project`. 0 on success, -1 on error. */
   int db2_kb_purge_fence_write(const char *project, const char *generation, const char *purge_id);

   /* Read the current fence. Returns 1 when a fence row exists (fills gen/pid;
    * *live_out = 1 iff the heartbeat is younger than TTL/3 — 2x the expected
    * heartbeat interval), 0 when absent, -1 on error. */
   int db2_kb_purge_fence_read(const char *project, char *gen_out, size_t gen_cap, char *pid_out,
                               size_t pid_cap, int *live_out);

   /* Writer-side commit-point check: 1 iff a fence row exists AND its heartbeat
    * is within the TTL. Expired or partially written fences count as absent. */
   int db2_kb_purge_fence_active(const char *project);

   /* Refresh the heartbeat iff BOTH generation and purge_id match the stored
    * fence. 1 refreshed, 0 mismatch/absent (no-op), -1 error. */
   int db2_kb_purge_fence_heartbeat(const char *project, const char *generation,
                                    const char *purge_id);

   /* Clear both fence rows iff BOTH generation and purge_id match. 1 cleared,
    * 0 mismatch/absent (no-op), -1 error. */
   int db2_kb_purge_fence_clear(const char *project, const char *generation, const char *purge_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_KB_RUNTIME_STATE_H */
