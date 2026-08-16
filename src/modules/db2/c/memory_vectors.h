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

/* Which of `ids` say the same thing as another of `ids`?
 *
 * Answers the near-duplicate question for one already-retrieved candidate set,
 * using the embeddings the rows ALREADY have (memories are embedded at write
 * time) -- so this costs a single query, not an embedder call, and the cosine is
 * computed by pgvector where the vectors live rather than shipping them into C.
 *
 * Writes canonical pairs into a_out/b_out with their cosine, closest first, and
 * returns the pair count (0 when nothing is similar enough, -1 on a bad call or
 * no connection). The caller decides what to do with a pair; this reports, it
 * does not suppress.
 *
 * Rows with no vector simply do not appear in any pair -- a memory written but
 * not yet embedded is absent from the collection, and the honest answer for it
 * is "unknown", not "distinct". Callers that must handle those rows need their
 * own fallback. */
int pgvec_memory_vector_near_duplicate_pairs(const int64_t *ids, int n, double min_cosine,
                                             int64_t *a_out, int64_t *b_out, double *cosine_out,
                                             int max);

#endif /* DEC_DB2_MEMORY_VECTORS_H */
