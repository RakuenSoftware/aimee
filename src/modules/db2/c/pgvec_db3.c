/* pgvec_db3.c: the process's DB3 routes, and the pgvec adapter over them. */

#include "pgvec_db3.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* One slot per routable collection. The set is closed and small, so the table
 * is a fixed array rather than a map: a collection this build does not know is
 * a programming error, and failing the lookup is how it surfaces instead of
 * silently creating a route nothing ever selects a provider for. */
typedef struct
{
   const char *collection;
   aimee_db3_route_t route;
   int installed;
} pgvec_db3_slot_t;

static pthread_mutex_t g_routes_mu = PTHREAD_MUTEX_INITIALIZER;
static pgvec_db3_slot_t g_routes[] = {
    {PGVEC_DB3_COLLECTION_MEMORY, {0}, 0},
    {PGVEC_DB3_COLLECTION_CODE, {0}, 0},
    {PGVEC_DB3_COLLECTION_KB, {0}, 0},
    {PGVEC_DB3_COLLECTION_KB_PDF, {0}, 0},
    {PGVEC_DB3_COLLECTION_CURATOR_ENTITY, {0}, 0},
    {PGVEC_DB3_COLLECTION_CURATOR_NARRATIVE, {0}, 0},
    {PGVEC_DB3_COLLECTION_CURATOR_CLAIM_SUBJ, {0}, 0},
    {PGVEC_DB3_COLLECTION_CURATOR_CLAIM_VAL, {0}, 0},
    {PGVEC_DB3_COLLECTION_CURATOR_CODE_INT, {0}, 0},
    {PGVEC_DB3_COLLECTION_CURATOR_CODE_SIG, {0}, 0},
    {PGVEC_DB3_COLLECTION_CURATOR_CODE_BODY, {0}, 0},
};

#define PGVEC_DB3_SLOT_COUNT (sizeof(g_routes) / sizeof(g_routes[0]))

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

/* Caller holds g_routes_mu. */
static pgvec_db3_slot_t *slot_for(const char *collection)
{
   if (!collection || !collection[0])
      return NULL;
   for (size_t index = 0; index < PGVEC_DB3_SLOT_COUNT; index++)
      if (strcmp(g_routes[index].collection, collection) == 0)
         return &g_routes[index];
   return NULL;
}

int pgvec_db3_route_install(const char *collection, aimee_db3_search_fn internal_search,
                            void *internal_context, aimee_db3_candidate_authorize_fn authorize,
                            void *authorize_context)
{
   if (!internal_search || !authorize)
      return -1;

   pthread_mutex_lock(&g_routes_mu);
   pgvec_db3_slot_t *slot = slot_for(collection);
   int rc = -1;
   if (slot)
   {
      /* Any previously selected provider was selected against the callbacks
       * being replaced, so it is dropped rather than carried across. */
      rc = aimee_db3_route_init(&slot->route, internal_search, internal_context, authorize,
                                authorize_context);
      slot->installed = (rc == 0);
   }
   pthread_mutex_unlock(&g_routes_mu);
   return rc;
}

int pgvec_db3_route_select(const char *collection, uint32_t principal, int ready,
                           int fallback_enabled, aimee_db3_search_fn external_search,
                           void *external_context)
{
   pthread_mutex_lock(&g_routes_mu);
   pgvec_db3_slot_t *slot = slot_for(collection);
   int rc = -1;
   if (slot && slot->installed)
      rc = aimee_db3_route_select(&slot->route, principal, ready, fallback_enabled, external_search,
                                  external_context);
   pthread_mutex_unlock(&g_routes_mu);
   return rc;
}

void pgvec_db3_route_clear(const char *collection)
{
   pthread_mutex_lock(&g_routes_mu);
   if (!collection)
   {
      for (size_t index = 0; index < PGVEC_DB3_SLOT_COUNT; index++)
         if (g_routes[index].installed)
            aimee_db3_route_clear(&g_routes[index].route);
   }
   else
   {
      pgvec_db3_slot_t *slot = slot_for(collection);
      if (slot && slot->installed)
         aimee_db3_route_clear(&slot->route);
   }
   pthread_mutex_unlock(&g_routes_mu);
}

int pgvec_db3_route_serving(const char *collection)
{
   pthread_mutex_lock(&g_routes_mu);
   pgvec_db3_slot_t *slot = slot_for(collection);
   int serving =
       slot && slot->installed && slot->route.selected_principal != 0 && slot->route.selected_ready;
   pthread_mutex_unlock(&g_routes_mu);
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

int pgvec_db3_candidates(const char *collection, const float *vec, int dim, const char *record_type,
                         const char *workspace, const char *project,
                         const pgvec_db3_filter_t *filters, int filter_count, int limit,
                         int64_t *ids, double *scores, int max)
{
   if (filter_count < 0 || filter_count > (int)AIMEE_DB3_MAX_LABELS ||
       (filter_count > 0 && !filters))
      return -1;
   if (!vec || dim <= 0 || dim > (int)AIMEE_DB3_MAX_DIM || !ids || !scores || max <= 0)
      return -1;

   int top_k = (limit > 0 && limit < max) ? limit : max;
   if (top_k <= 0 || top_k > (int)AIMEE_DB3_MAX_TOP_K)
      return -1;

   /* A search must be scoped by something. A filter narrows just as a workspace
    * does -- the curator collections scope by scope_kind and scope_id, which are
    * labels rather than either fixed field -- but a search with neither would
    * ask a provider to return its whole corpus. */
   if ((!workspace || !workspace[0]) && (!project || !project[0]) && filter_count == 0)
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

   /* Filters are copied verbatim, never reordered or de-duplicated here. The
    * encoder requires strictly ascending keys and refuses anything else, so a
    * caller that gets the order wrong is told so rather than having a different
    * question silently sent on its behalf. */
   for (int index = 0; index < filter_count; ++index)
   {
      if (!filters[index].key || !filters[index].value)
         return -1;
      if (copy_scope(request.filters[index].key, sizeof(request.filters[index].key),
                     filters[index].key) != 0 ||
          copy_scope(request.filters[index].value, sizeof(request.filters[index].value),
                     filters[index].value) != 0)
         return -1;
   }
   request.filter_count = (uint32_t)filter_count;

   aimee_db3_search_outcome_t outcome;
   pthread_mutex_lock(&g_routes_mu);
   pgvec_db3_slot_t *slot = slot_for(collection);
   int installed = slot && slot->installed;
   aimee_db3_result_t result = AIMEE_DB3_UNAVAILABLE;
   if (installed)
      result = aimee_db3_memory_candidates_search(&slot->route, &request, &outcome);
   pthread_mutex_unlock(&g_routes_mu);

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
