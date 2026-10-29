/* vector_index_ops.h: shared status bookkeeping for pgvector writes.
 *
 * pgvector (inside KB_STORE) owns the actual vector transport. KB_STORE owns the
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
   } kb_store_vector_index_ops_summary_t;

   typedef struct
   {
      int64_t point_id;
      char collection[64];
      int64_t memory_id;
      int attempts;
      char last_error[256];
      char updated_at[64];
   } kb_store_vector_index_op_failed_t;

   /* Upsert an ok/failed record for a point.  Bumps attempts and sets
    * last_error / indexed_at as appropriate. */
   void kb_store_vector_index_op_record(int64_t point_id, const char *collection, int64_t memory_id,
                                        int ok, const char *error_msg);

   /* Remove the row for a point (after e.g. a delete confirms landed). */
   void kb_store_vector_index_op_remove(int64_t point_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_VECTOR_INDEX_OPS_H */
