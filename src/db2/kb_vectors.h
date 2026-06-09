#ifndef DEC_DB2_KB_VECTORS_H
#define DEC_DB2_KB_VECTORS_H 1

#include <stdint.h>

const char *pgvec_kb_vector_collection_name(void);
int pgvec_kb_vector_upsert_document(int64_t doc_id, const float *vec, int dim,
                                    const char *payload_json);
int pgvec_kb_vector_upsert_document_batch(const int64_t *doc_ids, const float *vecs, int dim,
                                          const char *const *payloads, int count);
int pgvec_kb_vector_delete_point(int64_t point_id);
int pgvec_kb_vector_delete_project(const char *project);
int pgvec_kb_vector_search_project(const char *project, const float *vec, int dim, int limit,
                                   int64_t *ids, double *scores, int max);

#endif /* DEC_DB2_KB_VECTORS_H */
