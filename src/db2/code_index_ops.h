/* code_index_ops.h: DB2-side replay bookkeeping for code-chunk pgvector writes.
 *
 * Parallel to vector_index_ops (which tracks memory/evidence vector writes):
 * the code embed-at-scan path records one row per code point so failed embeds
 * are retried by `memory repair --reset-stuck` instead of being orphaned.
 * Pure domain API; no transport/backend handles leak. */
#ifndef DEC_CODE_INDEX_OPS_H
#define DEC_CODE_INDEX_OPS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int64_t ok_ops;
      int64_t pending_ops;
      int64_t failed_ops;
      int64_t stuck_ops;
   } db2_code_index_ops_summary_t;

   /* Upsert an ok/failed record for a code point. Bumps attempts; sets
    * last_error / indexed_at as appropriate. */
   void db2_code_index_op_record(int64_t point_id, const char *project, const char *node_key,
                                 const char *file_path, int ok, const char *error_msg);

   /* Reset attempts=0 on failed rows whose attempts hit `max_attempts`.
    * Returns the number of rows reset. */
   int db2_code_index_ops_reset_stuck(int max_attempts);

   /* Summary counts (stuck_ops uses max_attempts as the >= threshold). */
   int db2_code_index_ops_summary(int max_attempts, db2_code_index_ops_summary_t *out);

   /* auditable-correctness D7: count code embeddings whose source file was
    * re-scanned after the embedding was written (files.scanned_at >
    * code_embeddings.updated_at) — a staleness heuristic for ranking re-ingest.
    * Read-only; returns 0 on error / no candidates. */
   int64_t db2_code_index_drift_candidates(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CODE_INDEX_OPS_H */
