#include "memory_vectors.h"
#include "pgvec_transport.h"
#include "db2_internal.h" /* db2_conn */
#include "db_postgres.h"  /* aimee_pg_* */

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

int pgvec_memory_vector_near_duplicate_pairs(const int64_t *ids, int n, double min_cosine,
                                             int64_t *a_out, int64_t *b_out, double *cosine_out,
                                             int max)
{
   if (!ids || n <= 1 || !a_out || !b_out || !cosine_out || max <= 0)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   /* An explicit id list rather than a parameter array: the pg wrapper binds
    * scalars, and this set is small and bounded by the caller's candidate cap, so
    * the list is short. Ids are int64 read from our own rows, never user text. */
   char in_list[64 * 24];
   int pos = 0;
   for (int i = 0; i < n; i++)
   {
      int wrote = snprintf(in_list + pos, sizeof(in_list) - (size_t)pos, "%s%lld", i ? "," : "",
                           (long long)ids[i]);
      if (wrote <= 0 || (size_t)(pos + wrote) >= sizeof(in_list))
         break; /* bounded: compare the prefix that fits rather than overflow */
      pos += wrote;
   }
   if (pos == 0)
      return 0;

   /* a.point_id < b.point_id yields each unordered pair once. Unlike the kNN
    * self-join used for code similarity, this is an exhaustive comparison within
    * a small explicit set, so it cannot drop one-directional pairs and needs no
    * C-side dedup. */
   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT a.point_id, b.point_id, 1.0 - (a.embedding <=> b.embedding) AS cosine"
            " FROM %s a JOIN %s b ON a.point_id < b.point_id"
            " WHERE a.point_id IN (%s) AND b.point_id IN (%s)"
            "   AND 1.0 - (a.embedding <=> b.embedding) >= :minc"
            " ORDER BY cosine DESC LIMIT :lim",
            PGVEC_MEMORY_TABLE, PGVEC_MEMORY_TABLE, in_list, in_list);

   char errbuf[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return 0; /* no vector collection yet: report nothing, never fail the turn */
   aimee_pg_bind_double(stmt, ":minc", min_cosine);
   aimee_pg_bind_int(stmt, ":lim", max);

   int count = 0;
   while (count < max && aimee_pg_step(stmt, errbuf, sizeof(errbuf)) == AIMEE_PG_ROW)
   {
      a_out[count] = aimee_pg_column_int64(stmt, 0);
      b_out[count] = aimee_pg_column_int64(stmt, 1);
      cosine_out[count] = aimee_pg_column_double(stmt, 2);
      count++;
   }
   aimee_pg_finalize(stmt);
   return count;
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
