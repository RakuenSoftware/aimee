#include "memory_vectors.h"
#include "pgvec_transport.h"

#include <stddef.h>
#include <stdio.h>

static __thread char scope_hint_workspace[128];
static __thread char scope_hint_project[128];

void pgvec_memory_vector_scope_hint_set(const char *workspace, const char *project)
{
   scope_hint_workspace[0] = '\0';
   scope_hint_project[0] = '\0';
   if (workspace && workspace[0])
      snprintf(scope_hint_workspace, sizeof(scope_hint_workspace), "%s", workspace);
   if (project && project[0])
      snprintf(scope_hint_project, sizeof(scope_hint_project), "%s", project);
}

void pgvec_memory_vector_scope_hint_clear(void)
{
   scope_hint_workspace[0] = '\0';
   scope_hint_project[0] = '\0';
}

int pgvec_memory_vector_collection_exists(void)
{
   return pgvec_table_ready(PGVEC_MEMORY_TABLE);
}

int pgvec_memory_vector_collection_recreate(int dim)
{
   return pgvec_ensure_index(PGVEC_MEMORY_TABLE, dim, 1);
}

int pgvec_memory_vector_ensure_payload_indexes(void)
{
   return 0; /* payload columns are regular btree indexes created by schema.sql */
}

const char *pgvec_memory_vector_collection_name(void)
{
   return PGVEC_MEMORY_TABLE;
}

int pgvec_memory_vector_upsert_memory(int64_t memory_id, const float *vec, int dim,
                                      const char *payload_json)
{
   if (!vec || dim <= 0)
      return 0;
   return pgvec_memory_upsert(memory_id, vec, dim, payload_json);
}

int pgvec_memory_vector_upsert_unit(int64_t unit_id, const float *vec, int dim,
                                    const char *payload_json)
{
   if (!vec || dim <= 0)
      return 0;
   int64_t point_id = PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET + unit_id;
   return pgvec_memory_upsert(point_id, vec, dim, payload_json);
}

int pgvec_memory_vector_delete_point(int64_t point_id)
{
   return pgvec_memory_delete(point_id);
}

int pgvec_memory_vector_search_record_type(const char *record_type, const float *vec, int dim,
                                           int limit, int64_t *ids, double *scores, int max)
{
   return pgvec_memory_search(vec, dim, record_type, NULL, 0, scope_hint_workspace,
                              scope_hint_project, limit, ids, scores, max);
}

int pgvec_memory_vector_search_with_kinds(const float *vec, int dim, const char *const *kinds,
                                          int n_kinds, int limit, int64_t *ids, double *scores,
                                          int max)
{
   return pgvec_memory_search(vec, dim, "memory", kinds, n_kinds, scope_hint_workspace,
                              scope_hint_project, limit, ids, scores, max);
}
