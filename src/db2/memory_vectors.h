#ifndef DEC_DB2_MEMORY_VECTORS_H
#define DEC_DB2_MEMORY_VECTORS_H 1

#include <stdint.h>

#define PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET 1000000000000LL

int pgvec_memory_vector_collection_exists(void);
int pgvec_memory_vector_collection_recreate(int dim);
int pgvec_memory_vector_ensure_payload_indexes(void);
const char *pgvec_memory_vector_collection_name(void);
int pgvec_memory_vector_upsert_memory(int64_t memory_id, const float *vec, int dim,
                                      const char *payload_json);
int pgvec_memory_vector_upsert_unit(int64_t unit_id, const float *vec, int dim,
                                    const char *payload_json);
int pgvec_memory_vector_delete_point(int64_t point_id);
int pgvec_memory_vector_search_record_type(const char *record_type, const float *vec, int dim,
                                           int limit, int64_t *ids, double *scores, int max);
int pgvec_memory_vector_search_with_kinds(const float *vec, int dim, const char *const *kinds,
                                          int n_kinds, int limit, int64_t *ids, double *scores,
                                          int max);
void pgvec_memory_vector_scope_hint_set(const char *workspace, const char *project);
void pgvec_memory_vector_scope_hint_clear(void);

#endif /* DEC_DB2_MEMORY_VECTORS_H */
