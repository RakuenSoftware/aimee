#ifndef DEC_DB2_EVIDENCE_VECTORS_H
#define DEC_DB2_EVIDENCE_VECTORS_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif
   typedef struct
   {
      char artifact_id[37];
      char collection[64];
   } db2_evidence_pending_t;

   /* Enqueue an evidence artifact for embedding (idempotent — ON CONFLICT DO
    * NOTHING; status defaults to 'pending'). Returns 0 on success, -1 error. */
   int db2_evidence_enqueue(const char *artifact_id, const char *collection);

   /* Store the computed embedding for an artifact and mark its op 'ok'.
    * embedding_text is the JSON/pgvector text form. Returns 0/-1. */
   int db2_evidence_store_vector(const char *artifact_id, const char *collection,
                                 const char *embedding_text);

   /* Mark an op failed: status='failed', attempts=attempts+1, last_error=error.
    * Returns 0/-1. */
   int db2_evidence_mark_failed(const char *artifact_id, const char *error);

   /* List up to max pending ops (status='pending'), ordered by artifact_id.
    * Returns count written (>=0), or -1 on error. */
   int db2_evidence_list_pending(db2_evidence_pending_t *out, int max);

   /* Reset failed ops with attempts >= max_attempts back to pending (attempts=0).
    * Returns number of rows reset (>=0). */
   int db2_evidence_reset_stuck(int max_attempts);

   /* Re-embed the whole evidence layer: reset every op back to 'pending' so the
    * embed worker recomputes its vector (used on an embedding model_version
    * bump). store_vector overwrites in place, so no duplicate rows result.
    * Returns the number of ops reset (>=0). */
   int db2_evidence_reembed_all(void);

   /* Count ops with the given status (status may be NULL = all). Returns count
    * (>=0), or -1 on error. */
   int db2_evidence_ops_count(const char *status);

   /* One stored evidence vector joined to its artifact's kind. `embedding` is
    * the pgvector/JSON text form ("[f0,f1,...]"). */
   typedef struct
   {
      char artifact_id[37];
      char kind[64];
      char collection[64];
      char embedding[8192];
   } db2_evidence_vector_row_t;

   /* List up to `max` stored evidence vectors (joined to artifacts for kind),
    * ordered by artifact_id. Returns count written (>=0), or -1 on error. This
    * is the brute-force-scan source for the C-side neighbourhood ranker; a
    * pgvector-native top-K (ORDER BY embedding <=> q) can replace the ranker at
    * scale without changing learning_bundle's contract. */
   int db2_evidence_vectors_list(db2_evidence_vector_row_t *out, int max);
#ifdef __cplusplus
}
#endif
#endif