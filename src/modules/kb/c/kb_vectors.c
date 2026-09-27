#include "kb_vectors.h"
#include "pgvec_transport.h"

#include <stddef.h>

const char *pgvec_kb_vector_collection_name(void)
{
   return PGVEC_KB_TABLE;
}

int pgvec_kb_vector_upsert_document(int64_t doc_id, const float *vec, int dim,
                                    const char *payload_json)
{
   return pgvec_kb_upsert(doc_id, vec, dim, payload_json);
}

int pgvec_kb_vector_upsert_document_batch(const int64_t *doc_ids, const float *vecs, int dim,
                                          const char *const *payloads, int count)
{
   return pgvec_kb_upsert_batch(doc_ids, vecs, dim, payloads, count);
}

int pgvec_kb_vector_delete_point(int64_t point_id)
{
   return pgvec_kb_delete(point_id);
}

int pgvec_kb_vector_delete_project(const char *project)
{
   return pgvec_kb_delete_project(project);
}

int pgvec_kb_vector_delete_current_project(const char *project)
{
   return pgvec_kb_delete_current_project(project);
}

int pgvec_kb_vector_search_project(const char *project, const float *vec, int dim, int limit,
                                   int64_t *ids, double *scores, int max)
{
   return pgvec_kb_vector_search_scoped(project, NULL, vec, dim, limit, ids, scores, max);
}

int pgvec_kb_vector_search_scoped(const char *project, const char *exclude_project,
                                  const float *vec, int dim, int limit, int64_t *ids,
                                  double *scores, int max)
{
   return pgvec_kb_search_scoped(project, exclude_project, vec, dim, limit, ids, scores, max);
}
