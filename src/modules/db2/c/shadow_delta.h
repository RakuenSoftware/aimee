/* src/modules/db2/c/shadow_delta.h: Phase 7 shadow-mode rank-delta persistence.
 *
 * Backs memory_recall_shadow_deltas (created in schema.sql, Phase 0).
 * Stores compact per-query rank/score deltas so the eval harness can compare
 * fused vs fusion-off ranking without persisting full result payloads. */

#ifndef DEC_DB2_SHADOW_DELTA_H
#define DEC_DB2_SHADOW_DELTA_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Default retention bounds (proposal: memory.recall.shadow_delta_*). */
#define SHADOW_DELTA_DEFAULT_MAX_ROWS       10000
#define SHADOW_DELTA_DEFAULT_RETENTION_DAYS 14

   typedef struct
   {
      char query_hash[128];
      char project[256];
      char mode[32]; /* "shadow" | "on" */
      int64_t result_count;
      const char *delta_json; /* compact: id/key, baseline rank, fused rank, score delta */
   } db2_shadow_delta_row_t;

   /* Insert one shadow-delta row.  Returns 0 on success, -1 on error. */
   int db2_shadow_delta_insert(const db2_shadow_delta_row_t *row);

   /* Count shadow-delta rows for a project (or all if project NULL/empty). */
   int64_t db2_shadow_delta_count(const char *project);

   /* Enforce retention for a project: delete rows older than retention_days,
    * then trim to the newest max_rows.  When project is NULL/empty, applies
    * globally.  Pass 0 for either bound to use the SHADOW_DELTA_DEFAULT_*.
    * Returns the number of rows deleted, or -1 on error. */
   int db2_shadow_delta_cleanup(const char *project, int max_rows, int retention_days);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_SHADOW_DELTA_H */
