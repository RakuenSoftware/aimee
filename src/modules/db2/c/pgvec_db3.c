/* pgvec_db3.c: the process's DB3 route, and the pgvec adapter over it. */

#include "pgvec_db3.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* The process holds one route. Selection is deployment-wide — which provider
 * serves portable reads is not a per-call choice — so a single guarded instance
 * is the whole state, and searches take it under a read-mostly lock rather than
 * copying a route per call. */
static pthread_mutex_t g_route_mu = PTHREAD_MUTEX_INITIALIZER;
static aimee_db3_route_t g_route;
static int g_route_installed;

/* Request ids are per-call and need only be unique and nonzero within the
 * process; the wire uses them to pair a reply with its request. Relaxed because
 * nothing branches on the value, only on its distinctness. */
static _Atomic uint64_t g_request_id = 1;

static uint64_t next_request_id(void)
{
   uint64_t id = atomic_fetch_add_explicit(&g_request_id, 1, memory_order_relaxed);
   /* Zero is the wire's "unset". After 2^64 searches the counter wraps onto it,
    * so it is skipped rather than left to fail validation. */
   return id == 0 ? atomic_fetch_add_explicit(&g_request_id, 1, memory_order_relaxed) : id;
}

int pgvec_db3_route_install(aimee_db3_search_fn internal_search, void *internal_context,
                            aimee_db3_candidate_authorize_fn authorize, void *authorize_context)
{
   if (!internal_search || !authorize)
      return -1;

   pthread_mutex_lock(&g_route_mu);
   /* Any previously selected provider was selected against the callbacks being
    * replaced, so it is dropped rather than carried across. */
   int rc = aimee_db3_route_init(&g_route, internal_search, internal_context, authorize,
                                 authorize_context);
   g_route_installed = (rc == 0);
   pthread_mutex_unlock(&g_route_mu);
   return rc;
}

int pgvec_db3_route_select(uint32_t principal, int ready, int fallback_enabled,
                           aimee_db3_search_fn external_search, void *external_context)
{
   pthread_mutex_lock(&g_route_mu);
   int rc = -1;
   if (g_route_installed)
      rc = aimee_db3_route_select(&g_route, principal, ready, fallback_enabled, external_search,
                                  external_context);
   pthread_mutex_unlock(&g_route_mu);
   return rc;
}

void pgvec_db3_route_clear(void)
{
   pthread_mutex_lock(&g_route_mu);
   if (g_route_installed)
      aimee_db3_route_clear(&g_route);
   pthread_mutex_unlock(&g_route_mu);
}

int pgvec_db3_route_serving(void)
{
   pthread_mutex_lock(&g_route_mu);
   int serving = g_route_installed && g_route.selected_principal != 0 && g_route.selected_ready;
   pthread_mutex_unlock(&g_route_mu);
   return serving;
}

/* Copy a scope component, refusing one that would not survive the wire.
 * Truncating a workspace name would search a different workspace, so an
 * over-long value fails the search rather than being cut down. */
static int copy_scope(char *out, size_t capacity, const char *value)
{
   if (!value)
      value = "";
   size_t len = strlen(value);
   if (len >= capacity)
      return -1;
   memcpy(out, value, len + 1);
   return 0;
}

int pgvec_db3_memory_candidates(const float *vec, int dim, const char *record_type,
                                const char *workspace, const char *project, int limit,
                                int64_t *ids, double *scores, int max)
{
   if (!vec || dim <= 0 || dim > (int)AIMEE_DB3_MAX_DIM || !record_type || !ids || !scores || max <= 0)
      return -1;

   int top_k = (limit > 0 && limit < max) ? limit : max;
   if (top_k <= 0 || top_k > (int)AIMEE_DB3_MAX_TOP_K)
      return -1;

   /* An unscoped search cannot be expressed on the wire, and sending one with
    * both components blank would ask a provider to search everything. */
   if ((!workspace || !workspace[0]) && (!project || !project[0]))
      return -1;

   aimee_db3_search_request_t request;
   memset(&request, 0, sizeof(request));
   request.request_id = next_request_id();
   /* The route's generation gating is the provider selector's business, not
    * this call's: a required generation of 1 means "whatever the selected
    * provider is serving", and selection is what guarantees that is current. */
   request.required_generation = 1;
   request.dimension = (uint32_t)dim;
   request.top_k = (uint32_t)top_k;
   if (copy_scope(request.workspace, sizeof(request.workspace), workspace) != 0 ||
       copy_scope(request.project, sizeof(request.project), project) != 0 ||
       copy_scope(request.record_type, sizeof(request.record_type), record_type) != 0)
      return -1;
   memcpy(request.vector, vec, (size_t)dim * sizeof(float));

   aimee_db3_search_outcome_t outcome;
   pthread_mutex_lock(&g_route_mu);
   int installed = g_route_installed;
   aimee_db3_result_t result = AIMEE_DB3_UNAVAILABLE;
   if (installed)
      result = aimee_db3_memory_candidates_search(&g_route, &request, &outcome);
   pthread_mutex_unlock(&g_route_mu);

   if (!installed || result != AIMEE_DB3_OK)
      return -1;

   uint32_t count = outcome.reply.count;
   if (count > (uint32_t)max)
      count = (uint32_t)max;
   for (uint32_t index = 0; index < count; index++)
   {
      ids[index] = outcome.reply.candidates[index].point_id;
      scores[index] = outcome.reply.candidates[index].score;
   }
   return (int)count;
}
