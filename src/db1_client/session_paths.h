/* db1/session_paths.h: per-session write-path log + parent<->child
 * stale-read detector for the cross-delegate stale-read warning in
 * docs/proposals/pending/delegate-reliability-heartbeat-and-cost-rollup.md
 * (Phase 4b).
 *
 * Read paths are already tracked by the session_state_read_paths table
 * via the existing branch-ownership read-tracking. This module adds the
 * write side and the comparator: when a child delegate writes a file
 * the parent had previously read, the parent's snapshot of that file
 * is stale, and we want to surface that in the child's summary so the
 * parent re-reads before editing.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_SESSION_PATHS_H
#define DEC_DB1_SESSION_PATHS_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_SESSION_PATH_LEN 512

   /* Append a file write to the per-session write log. Returns 0 on
    * success, -1 on error. Idempotent at the path level — repeated
    * writes to the same path get one row each (seq monotonic) so the
    * intersection helper does not double-count by path; intersection is
    * by DISTINCT path. */
   int db1_session_write_path_record(const char *session_id, const char *path);

   /* Find every distinct path that the parent session has READ AND that
    * the child session has WRITTEN. Writes up to `max` paths into
    * `out_paths`, each a NUL-terminated string of length up to
    * DB1_SESSION_PATH_LEN. Returns the count written, 0 on no overlap,
    * -1 on error. */
   int db1_session_stale_reads(const char *parent_session_id, const char *child_session_id,
                               char (*out_paths)[DB1_SESSION_PATH_LEN], int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_SESSION_PATHS_H */
