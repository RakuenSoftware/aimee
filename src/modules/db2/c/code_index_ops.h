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

   /* auditable-correctness D7 requeue: enqueue each distinct drifted project (one
    * with >=1 drift candidate, deduped against an already pending/running queue
    * row) into kb_ingest_queue with force, so the ingest drain re-embeds it.
    * MUTATING — do not call under dry_run. Returns the number enqueued (0 on
    * error / nothing to do). */
   int db2_code_index_requeue_drifted(void);

   /* auditable-correctness P1.5/D8: resolve a code ref's LIVE source hash —
    * files.hash for (project, file_path). Writes the hash into out (cleared first);
    * the CALLER compares it to the turn-captured version to flag drift. Returns
    * 1 (found), 0 (no such file), -1 (bad arg / error). */
   int db2_code_file_hash(const char *project, const char *file_path, char *out, int out_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CODE_INDEX_OPS_H */
