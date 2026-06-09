#ifndef DEC_DB2_PGVEC_KB_SERVICE_H
#define DEC_DB2_PGVEC_KB_SERVICE_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int rc;
      int64_t mem_pruned;
      int64_t mem_kept;
      int64_t kb_pruned;
      int64_t kb_kept;
   } pgvec_kb_service_reconcile_result_t;

   typedef int (*pgvec_kb_service_record_exists_fn)(int64_t record_id);

   int pgvec_kb_service_ensure_kb_collection(int dim);
   int pgvec_kb_service_ensure_memory_collection(int dim);

   int pgvec_kb_service_reconcile_orphans(pgvec_kb_service_record_exists_fn mem_exists,
                                          pgvec_kb_service_record_exists_fn kb_exists, int dry_run,
                                          pgvec_kb_service_reconcile_result_t *out);

   int pgvec_kb_service_search_memory_points(const char *record_type, const float *vec, int dim,
                                             int limit, int64_t *ids, double *scores, int max);
   int pgvec_kb_service_upsert_document_point(int64_t doc_id, const float *vec, int dim,
                                              const char *payload_json);

   /* Phase 5: code embedding helpers. */
   int pgvec_kb_service_ensure_code_collection(int dim);
   int pgvec_kb_service_code_upsert(int64_t point_id, const float *vec, int dim,
                                    const char *project, const char *node_key,
                                    const char *file_path, const char *symbol,
                                    const char *content_hash, const char *body_hash,
                                    const char *payload_json);
   int pgvec_kb_service_code_exists_by_hash(const char *project, const char *node_key,
                                            const char *content_hash, const char *body_hash);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_PGVEC_KB_SERVICE_H */
