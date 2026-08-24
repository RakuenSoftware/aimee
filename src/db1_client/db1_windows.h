/* db1/db1_windows.h: per-session conversation windows and local indexes.
 *
 * windows, window_terms, window_files, and window_fts form the local
 * session-turn index used by memory_scan, rewind, and retrieval.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_WINDOWS_H
#define DEC_DB1_WINDOWS_H 1

#include "aimee.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t window_id;
      char session_id[128];
      int seq;
      char summary[1024];
      char created_at[32];
      int match_count;
   } db1_window_search_candidate_t;

   typedef struct
   {
      int64_t window_id;
      double rank;
   } db1_window_lexical_hit_t;

   /* Read current scan state for one session. Missing session => 0/0. */
   int db1_windows_session_scan_state(const char *session_id, int *count_out, int *max_seq_out);

   /* Resolve one window id to its session id. Missing window => 0 with empty out buffer. */
   int db1_window_session_id(int64_t window_id, char *out, size_t out_sz);

   /* Insert one raw conversation window. Returns new row id (>0) or -1. */
   int64_t db1_window_create_raw(const char *session_id, int seq, const char *summary,
                                 const char *created_at);

   /* Add one indexed term/file row to an existing window. 0 on success. */
   int db1_window_add_term(int64_t window_id, const char *term);
   int db1_window_add_file(int64_t window_id, const char *file_path);

   /* List window ids by tier older than N days, ordered by id. */
   int db1_windows_list_ids_by_tier_before_days(const char *tier, int older_than_days,
                                                int64_t *out_ids, int max);

   /* Search relational window candidates by indexed terms. */
   int db1_windows_find_candidates_by_terms(const char *const *terms, int term_count,
                                            db1_window_search_candidate_t *out, int max);

   /* List file refs for one window in rowid order. */
   int db1_window_list_files(int64_t window_id, char out[][MAX_PATH_LEN], int max);

   /* Maintain/search the local window lexical index. */
   int db1_window_index_summary(int64_t window_id, const char *summary);
   int db1_windows_find_lexical_hits(const char *const *terms, int term_count,
                                     db1_window_lexical_hit_t *out, int max);

   /* Tier/child-row maintenance helpers used by compaction. */
   int db1_window_set_tier(int64_t window_id, const char *tier);
   int db1_window_prune_terms_keep_top(int64_t window_id, int keep);
   int db1_window_delete_all_files(int64_t window_id);
   int db1_window_prune_files_keep_top(int64_t window_id, int keep);

   /* Delete rows for session_id with seq > turn. Returns rows deleted, or -1. */
   int db1_windows_delete_after_turn(const char *session_id, int turn);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_WINDOWS_H */
