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

/* The route became shared the moment something started delivering announcements
 * to it: the provider registry, the selected principal and the ready flag are
 * now written by whichever thread the host observes the bus on, while searches
 * on every request thread read them.
 *
 * A rwlock rather than a mutex because the asymmetry is total. Announcements
 * arrive on a provider's timer -- seconds apart, and zero in the deployments
 * that have no provider at all. Searches are the product's hot path and are
 * concurrent by design; serialising them behind the writer's lock would make an
 * optional accelerator a global bottleneck for everyone who never attached one.
 *
 * Readers hold it across the whole search, not just the selection read. The
 * search consults the route repeatedly -- selected principal, ready flag,
 * fallback policy, the transport and authorisation callbacks -- and a selection
 * that changed midway would mix two providers' policy in one answer. */
static pthread_rwlock_t memory_route_lock = PTHREAD_RWLOCK_INITIALIZER;

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

/* ------------------------------------------------- provider announcements
 *
 * The delivery half of provider detection. Without it the registry below can
 * decide which provider should serve reads and never hears about one.
 */

/* Announcements this build could not read. A provider looping on a bad frame
 * would otherwise write a log line per announcement; what is worth knowing is
 * that this deployment is receiving announcements it cannot read, and that is a
 * number rather than a stream. */
static _Atomic uint64_t capabilities_rejected;

/* Announcements that reached this process at all.
 *
 * Separates the three states an operator has to tell apart: nothing announcing
 * (seen == 0), announcing in a dialect this build cannot read (rejected > 0),
 * and announcing fine but not eligible to serve reads (seen > 0, rejected == 0,
 * selected == 0). Without this, all three look identical from outside -- which
 * is exactly the position the first run of the observation test left me in. */
static _Atomic uint64_t capabilities_seen;

uint64_t pgvec_memory_vector_capabilities_seen(void)
{
   return atomic_load(&capabilities_seen);
}

uint64_t pgvec_memory_vector_capabilities_rejected(void)
{
   return atomic_load(&capabilities_rejected);
}

uint32_t pgvec_memory_vector_selected_provider(void)
{
   pthread_once(&memory_route_once, memory_route_init);
   if (memory_route_init_failed)
      return 0;
   pthread_rwlock_rdlock(&memory_route_lock);
   uint32_t selected = memory_route.selected_principal;
   pthread_rwlock_unlock(&memory_route_lock);
   return selected;
}

int pgvec_memory_vector_on_capabilities(uint32_t principal_ref, uint32_t src_handle,
                                        uint64_t sequence, const uint8_t *payload,
                                        uint32_t payload_len)
{
   pthread_once(&memory_route_once, memory_route_init);
   if (memory_route_init_failed)
      return -1;

   atomic_fetch_add(&capabilities_seen, 1);

   aimee_vector_capabilities_t capabilities;
   if (aimee_vector_capabilities_decode(payload, payload_len, &capabilities) != 0)
   {
      atomic_fetch_add(&capabilities_rejected, 1);
      return -1;
   }
   /* principal_ref and src_handle are the CALLER's statement of who sent this,
    * which the KB takes from the bus frame. Nothing here reads an identity out
    * of the payload: a provider that could name its own principal could name
    * somebody else's. */
   pthread_rwlock_wrlock(&memory_route_lock);
   int observed = aimee_vector_route_observe_capabilities(&memory_route, principal_ref, src_handle,
                                                          sequence, &capabilities);
   /* Read the resulting selection under the same lock that produced it: reported
    * separately it could name a provider a later announcement had already
    * replaced, which is worse than not logging it. */
   uint32_t selected = memory_route.selected_principal;
   pthread_rwlock_unlock(&memory_route_lock);
   if (observed != 0)
   {
      /* Stale within an attachment, or a full registry. Neither is malformed,
       * and neither is worth a log line per announcement. */
      return -1;
   }
   LOG_INFO("vector_provider",
            "capabilities principal=%u handle=%u ready=%d generation=%llu selected=%u",
            principal_ref, src_handle, capabilities.ready,
            (unsigned long long)capabilities.generation, selected);
   return 0;
}

/* ------------------------------------------------------------- transport
 *
 * The other half of provider detection. Selection decided WHO should serve
 * reads; this is how a search reaches them.
 *
 * db2 encodes and decodes because db2 owns the wire. It does not know what a bus
 * is: the host installs a byte-level call and this drives it. Same split as the
 * announcement path, for the same reason -- a dependency on the AUDIT module
 * here is one the standalone bundle would have to carry or replace.
 */

static pgvec_vector_call_fn transport_call;
static void *transport_context;

static _Atomic uint64_t provider_searches;
static _Atomic uint64_t provider_failures;

uint64_t pgvec_memory_vector_provider_searches(void)
{
   return atomic_load(&provider_searches);
}

uint64_t pgvec_memory_vector_provider_failures(void)
{
   return atomic_load(&provider_failures);
}

/* The route's external leg. Runs on the searching thread, under the route's read
 * lock -- so it must not touch the route, and does not: everything it needs is
 * in the request the route handed it. */
static int external_search(void *context, const aimee_vector_search_request_t *request,
                           aimee_vector_search_reply_t *reply)
{
   (void)context;
   if (!transport_call)
      return -1;

   /* Sized from the contract rather than guessed: a request is its header plus
    * the filter block plus four bytes per dimension, and a reply is its header
    * plus one candidate record per result. Stack buffers, because a search on
    * the hot path must not depend on an allocator being able to answer. */
   uint8_t request_bytes[AIMEE_VECTOR_SEARCH_REQUEST_HEADER + AIMEE_VECTOR_MAX_FILTER_BYTES +
                         4u * AIMEE_VECTOR_MAX_DIM];
   size_t request_len = 0;
   if (aimee_vector_search_request_encode(request, request_bytes, sizeof(request_bytes),
                                          &request_len) != 0)
   {
      atomic_fetch_add(&provider_failures, 1);
      return -1;
   }

   uint8_t reply_bytes[AIMEE_VECTOR_SEARCH_REPLY_HEADER +
                       AIMEE_VECTOR_CANDIDATE_BYTES * AIMEE_VECTOR_MAX_TOP_K];
   uint32_t reply_len = 0;
   if (transport_call(transport_context, AIMEE_VECTOR_EVENT_SEARCH, AIMEE_VECTOR_STAGE_SEARCH,
                      request_bytes, (uint32_t)request_len, reply_bytes, sizeof(reply_bytes),
                      &reply_len) != 0)
   {
      atomic_fetch_add(&provider_failures, 1);
      return -1;
   }
   if (aimee_vector_search_reply_decode(reply_bytes, reply_len, reply) != 0)
   {
      /* The provider answered with something this build cannot read. Counted as
       * a failure rather than an empty result: an empty result is an answer, and
       * this is the absence of one. */
      atomic_fetch_add(&provider_failures, 1);
      return -1;
   }
   atomic_fetch_add(&provider_searches, 1);
   return 0;
}

int pgvec_memory_vector_set_transport(pgvec_vector_call_fn call, void *context,
                                      int fallback_enabled)
{
   if (!call)
      return -1;
   pthread_once(&memory_route_once, memory_route_init);
   if (memory_route_init_failed)
      return -1;

   /* The write lock, because external_search reads transport_call and the route
    * reads its own external_search leg -- and both of those reads happen under
    * the read lock held across a search. Installing without it would publish a
    * half-installed transport to a search already running. */
   pthread_rwlock_wrlock(&memory_route_lock);
   int rc = -1;
   /* Once, like the observer: a second transport is an ownership question, and
    * swapping one under searches already in flight is not something any caller
    * needs. */
   if (!transport_call)
   {
      transport_call = call;
      transport_context = context;
      rc = aimee_vector_route_set_transport(&memory_route, external_search, NULL, fallback_enabled);
      if (rc != 0)
      {
         transport_call = NULL;
         transport_context = NULL;
      }
   }
   pthread_rwlock_unlock(&memory_route_lock);
   return rc;
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
      pthread_rwlock_rdlock(&memory_route_lock);
      uint32_t selected = memory_route_init_failed ? 0 : memory_route.selected_principal;
      pthread_rwlock_unlock(&memory_route_lock);
      if (selected != 0)
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
   pthread_rwlock_rdlock(&memory_route_lock);
   aimee_vector_result_t rc =
       aimee_vector_memory_candidates_search(&memory_route, &request, &outcome);
   pthread_rwlock_unlock(&memory_route_lock);
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
