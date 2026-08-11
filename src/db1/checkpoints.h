/* db1/checkpoints.h: session checkpoint storage — DB1 subsystem.
 *
 * Checkpoints are local-user/session rewind state. DB1 owns the table and
 * all SQLite access; callers use this typed API only. */
#ifndef DEC_DB1_CHECKPOINTS_H
#define DEC_DB1_CHECKPOINTS_H 1

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t id;
      int64_t task_id;
      char session_id[128];
      char label[256];
      char snapshot[8192];
      char created_at[32];
   } db1_checkpoint_t;

   int db1_checkpoint_insert(const char *label, const char *session_id, int64_t task_id,
                             const char *snapshot_json, db1_checkpoint_t *out);
   int db1_checkpoint_get(int64_t id, db1_checkpoint_t *out);
   int db1_checkpoint_list(int limit, db1_checkpoint_t *out, int max);
   int db1_checkpoint_delete(int64_t id);

   /* Per-conversation economizer state (context-paging S2c).
    *
    * Kept as a named checkpoint rather than a new table: it is exactly session-scoped
    * rewind state, which is what this table already is. The label is an internal
    * detail so callers cannot collide with it.
    *
    * Scalar-only signatures on purpose — the agent loop forward-declares its db1
    * entry points instead of including this header, and a db1_checkpoint_t in the
    * signature would force the struct across that boundary.
    *
    * STRICTLY session-keyed. Restoring one conversation's reducer state into another
    * would leak context between sessions; `session_id` must be non-empty or both
    * calls no-op. Save replaces: only the newest row per session is ever read, and
    * older ones are pruned so a long-lived session cannot grow the table without
    * bound.
    *
    * load() returns 0 and fills `out` (NUL-terminated, bounded by out_sz) on a hit;
    * -1 on miss, bad args, or a row that would not fit. */
   int db1_economizer_state_save(const char *session_id, const char *json);
   int db1_economizer_state_load(const char *session_id, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_CHECKPOINTS_H */
