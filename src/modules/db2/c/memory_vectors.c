#include "memory_vectors.h"
#include "pgvec_transport.h"
#include "db2_internal.h" /* db2_conn */
#include "db_postgres.h"  /* aimee_pg_* */
#include "log.h"

#include "aimee/db2/vector_route.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

/* ------------------------------------------------------------------ routing
 *
 * The one place a memory vector search is issued. With no provider selected the
 * route calls straight back into pgvec_memory_search below, so this is today's
 * behaviour with a seam in it; when a provider is selected it receives the
 * search and its results are authorised against this deployment's scope.
 */

static aimee_vector_route_t memory_route;
static pthread_once_t memory_route_once = PTHREAD_ONCE_INIT;
static int memory_route_init_failed;

/* Searches that were expressible in the contract and went through the route.
 *
 * The routed and direct paths return identical results by design, so this is
 * the only way to tell them apart from outside -- for a test proving the route
 * is used, and for an operator asking whether an attached provider is actually
 * seeing traffic or is being bypassed by every query. */
static _Atomic uint64_t routed_searches;

uint64_t pgvec_memory_vector_routed_searches(void)
{
   return atomic_load(&routed_searches);
}

/* Monotonic, process-wide, never 0. */
static uint64_t next_request_id(void)
{
   static _Atomic uint64_t counter;
   return atomic_fetch_add(&counter, 1) + 1;
}

/* What the wrapper passed, so the default leg can pass it on unchanged.
 *
 * Thread-local because the route is process-wide and searches are concurrent,
 * and because the scope this search runs under is itself thread-local: the
 * request and the context it must be answered in belong to the same thread. */
static __thread struct
{
   const char *const *kinds;
   int n_kinds;
   int limit;
   int max;
} internal_call;

static int internal_search(void *context, const aimee_vector_search_request_t *request,
                           aimee_vector_search_reply_t *reply)
{
   (void)context;
   int64_t ids[AIMEE_VECTOR_MAX_TOP_K];
   double scores[AIMEE_VECTOR_MAX_TOP_K];
   int cap = internal_call.max;
   if (cap > (int)AIMEE_VECTOR_MAX_TOP_K)
      cap = (int)AIMEE_VECTOR_MAX_TOP_K;

   /* The caller's own arguments, not a reconstruction of them: the scope this
    * runs under comes from the thread-local context pgvec_memory_search reads
    * for itself, on this thread, inside this call. */
   int n = pgvec_memory_search(request->vector, (int)request->dimension, request->record_type,
                               internal_call.kinds, internal_call.n_kinds, request->workspace,
                               request->project, internal_call.limit, ids, scores, cap);
   if (n < 0)
      return -1;

   reply->request_id = request->request_id;
   reply->generation = request->required_generation;
   reply->count = (uint32_t)n;
   for (int i = 0; i < n; ++i)
   {
      reply->candidates[i].point_id = ids[i];
      reply->candidates[i].score = scores[i];
   }
   return 0;
}

static int authorize_candidate(void *context, const char *workspace, const char *project,
                               int64_t point_id)
{
   (void)context;
   (void)workspace;
   (void)project;
   /* Scope comes from the thread-local context, the same one the internal
    * search reads -- the hints on the request are what a caller asked for, not
    * what the request is entitled to see. */
   return pgvec_memory_point_visible(point_id);
}

static void memory_route_init(void)
{
   if (aimee_vector_route_init(&memory_route, internal_search, NULL, authorize_candidate, NULL) !=
       0)
      memory_route_init_failed = 1;
}

/* Fill a request, or return -1 if this search is not expressible in the
 * contract. Nothing here is a failure: it is a search that must take the
 * direct path. */
static int build_request(aimee_vector_search_request_t *request, const char *record_type,
                         const float *vec, int dim, const char *const *kinds, int n_kinds,
                         int limit, int max, aimee_vector_filter_t *kind_filter)
{
   if (!record_type || strlen(record_type) >= AIMEE_VECTOR_MAX_RECORD_TYPE)
      return -1;
   if (strlen(scope_hint_workspace) >= AIMEE_VECTOR_MAX_SCOPE ||
       strlen(scope_hint_project) >= AIMEE_VECTOR_MAX_SCOPE)
      return -1;
   /* A request with no scope carries no tenancy statement, and a request with
    * no tenancy statement must not leave this deployment. The direct path is
    * the right answer, not a gap to close later. */
   if (!scope_hint_workspace[0] && !scope_hint_project[0])
      return -1;
   int top_k = limit > 0 ? limit : max;
   if (top_k <= 0 || top_k > (int)AIMEE_VECTOR_MAX_TOP_K)
      return -1;
   if (dim <= 0 || (uint32_t)dim > AIMEE_VECTOR_MAX_DIM)
      return -1;
   if (n_kinds < 0 || (size_t)n_kinds > AIMEE_VECTOR_MAX_FILTER_VALUES)
      return -1;
   for (int i = 0; i < n_kinds; ++i)
      if (!kinds[i] || !kinds[i][0] || strlen(kinds[i]) >= AIMEE_VECTOR_MAX_LABEL_VALUE)
         return -1;

   memset(request, 0, sizeof(*request));
   /* Pairs a reply with its request, which matters as soon as a provider
    * answers out of band. Starts at 1: 0 is the value the validator refuses. */
   request->request_id = next_request_id();
   /* Memory embeddings have one live index that re-embedding overwrites in
    * place, so there is exactly one generation and it is 1. It becomes a real
    * generation when memory grows the column the KB projections already have;
    * until then 1 is a fact, not a placeholder. Not 0 -- 0 is refused, and
    * spelling "not applicable" as the invalid value hides the day it matters. */
   request->required_generation = 1;
   snprintf(request->record_type, sizeof(request->record_type), "%s", record_type);
   snprintf(request->workspace, sizeof(request->workspace), "%s", scope_hint_workspace);
   snprintf(request->project, sizeof(request->project), "%s", scope_hint_project);
   request->dimension = (uint32_t)dim;
   request->top_k = (uint32_t)top_k;
   request->vector = vec;

   /* The kind restriction is the reason search version 2 exists: version 1 had
    * no way to say it, so a provider could not have served this search at all. */
   if (n_kinds > 0)
   {
      kind_filter->op = AIMEE_VECTOR_FILTER_IN;
      kind_filter->key = "kind";
      kind_filter->values = kinds;
      kind_filter->value_count = (size_t)n_kinds;
      request->filters = kind_filter;
      request->filter_count = 1;
   }
   return 0;
}

static int routed_search(const char *record_type, const float *vec, int dim,
                         const char *const *kinds, int n_kinds, int limit, int64_t *ids,
                         double *scores, int max)
{
   if (!vec || !ids || !scores || max <= 0)
      return -1;

   pthread_once(&memory_route_once, memory_route_init);

   aimee_vector_search_request_t request;
   aimee_vector_filter_t kind_filter;
   if (memory_route_init_failed ||
       build_request(&request, record_type, vec, dim, kinds, n_kinds, limit, max, &kind_filter) < 0)
   {
      /* A search the contract cannot carry still has to run. It says so louder
       * when a provider is selected, because then the deployment asked for
       * acceleration and is silently not getting it. */
      if (memory_route.selected_principal != 0)
         LOG_WARN("vector_route_bypass",
                  "search not expressible, using pgvector record_type=%.32s dim=%d limit=%d",
                  record_type ? record_type : "", dim, limit);
      return pgvec_memory_search(vec, dim, record_type, kinds, n_kinds, scope_hint_workspace,
                                 scope_hint_project, limit, ids, scores, max);
   }

   atomic_fetch_add(&routed_searches, 1);
   internal_call.kinds = kinds;
   internal_call.n_kinds = n_kinds;
   internal_call.limit = limit;
   internal_call.max = max;

   aimee_vector_search_outcome_t outcome;
   aimee_vector_result_t rc =
       aimee_vector_memory_candidates_search(&memory_route, &request, &outcome);
   if (rc != AIMEE_VECTOR_OK)
      return -1;

   int n = 0;
   for (uint32_t i = 0; i < outcome.reply.count && n < max; ++i)
   {
      ids[n] = outcome.reply.candidates[i].point_id;
      scores[n] = outcome.reply.candidates[i].score;
      n++;
   }
   return n;
}

int pgvec_memory_vector_search_record_type(const char *record_type, const float *vec, int dim,
                                           int limit, int64_t *ids, double *scores, int max)
{
   return routed_search(record_type, vec, dim, NULL, 0, limit, ids, scores, max);
}

int pgvec_memory_vector_search_with_kinds(const float *vec, int dim, const char *const *kinds,
                                          int n_kinds, int limit, int64_t *ids, double *scores,
                                          int max)
{
   return routed_search("memory", vec, dim, kinds, n_kinds, limit, ids, scores, max);
}
