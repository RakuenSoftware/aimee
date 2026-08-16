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
