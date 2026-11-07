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

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_WINDOWS_H */
