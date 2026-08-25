/* vector_index_ops.h: shared status bookkeeping for pgvector writes.
 *
 * pgvector (inside DB2) owns the actual vector transport. DB2 owns the
 * relational bookkeeping used to retry and report those writes.
 * Nothing here exposes backend handles or transport details.
 *
 * Pure domain API. Backend access stays private to the implementation.
 */
#ifndef DEC_VECTOR_INDEX_OPS_H
#define DEC_VECTOR_INDEX_OPS_H 1

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
      /* Write-to-readable lag: seconds between a record becoming durable
       * (memories.created_at) and becoming retrievable (indexed_at, once the
       * queued embed lands). Capture is synchronous and enrichment is queued, so
       * this interval is the window in which a just-stored memory is not yet
       * recallable -- the thing a user notices first as "it forgot what I just
       * said". The timestamps were already written; only the summary was
       * missing, so the property went unpriced.
       *
       * -1 means not computed (no landed rows in the window, or the query
       * failed); lag_samples carries how many rows the percentiles are over. */
      double lag_p50_secs;
      double lag_p90_secs;
      double lag_p95_secs;
      double lag_p99_secs;
      double lag_max_secs;
      int64_t lag_samples;
   } db2_vector_index_ops_summary_t;

   typedef struct
   {
      int64_t point_id;
      char collection[64];
      int64_t memory_id;
      int attempts;
      char last_error[256];
      char updated_at[64];
   } db2_vector_index_op_failed_t;

   /* Return 1 when DB2 is running in an isolated eval/benchmark mode where
    * pgvector side effects should be skipped. */
   int db2_vector_index_sync_suppressed(void);

   /* Upsert an ok/failed record for a point.  Bumps attempts and sets
    * last_error / indexed_at as appropriate. */
   void db2_vector_index_op_record(int64_t point_id, const char *collection, int64_t memory_id,
                                   int ok, const char *error_msg);

   /* Remove the row for a point (after e.g. a delete confirms landed). */
   void db2_vector_index_op_remove(int64_t point_id);

   /* Remove the rows belonging to `memory_id`, set-based so reclamation is not
    * bounded by a caller's buffer. Always covers one row per memory_unit
    * (id + `unit_point_offset`); `include_base` additionally covers the record's
    * own point and any row carrying its memory_id. Pass 0 from unit-rebuild
    * paths. Call while the memory_units rows still exist. */
   void db2_vector_index_ops_remove_for_memory(int64_t memory_id, int64_t unit_point_offset,
                                               int include_base);

   /* Reset attempts=0 on failed rows whose attempts hit `max_attempts`.
    * Returns the number of rows reset. */
   int db2_vector_index_ops_reset_stuck(int max_attempts);

   /* Summary counts for use by `memory verify` and maintenance jobs.
    * `stuck_ops` uses `max_attempts` as the ≥ threshold; pass the same
    * value the failed-replay retry logic uses. */
   int db2_vector_index_ops_summary(int max_attempts, db2_vector_index_ops_summary_t *out);

   /* Enumerate the top failed rows (limit `max_rows`) for diagnostics. */
   int db2_vector_index_ops_list_failed(db2_vector_index_op_failed_t *rows, int max_rows);

   /* List distinct memory_id rows from failed vector index operations with
    * status = 'failed' AND attempts < max_attempts, ordered by
    * updated_at ASC. limit <= 0 means unlimited (still capped by `max`).
    * Returns count written. */
   int db2_vector_index_ops_list_retryable_memory_ids(int max_attempts, int limit, int64_t *out,
                                                      int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_VECTOR_INDEX_OPS_H */
