/* test_pgvec_db3.c: the pgvec -> DB3 adapter.
 *
 * The route itself is covered by test_db3_route.c. What is tested here is the
 * adapter's own decisions: which searches it will express on the wire at all,
 * and how it translates a reply back into pgvec's id/score arrays.
 */
#include "../modules/db2/c/pgvec_db3.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   int calls;
   int fail;
   uint32_t last_top_k;
   uint32_t last_dimension;
   char last_workspace[AIMEE_DB3_MAX_SCOPE];
   char last_project[AIMEE_DB3_MAX_SCOPE];
   char last_record_type[AIMEE_DB3_MAX_RECORD_TYPE];
   uint64_t last_request_id;
} search_state_t;

static int fake_search(void *context, const aimee_db3_search_request_t *request,
                       aimee_db3_search_reply_t *reply)
{
   search_state_t *state = context;
   state->calls++;
   state->last_top_k = request->top_k;
   state->last_dimension = request->dimension;
   state->last_request_id = request->request_id;
   snprintf(state->last_workspace, sizeof(state->last_workspace), "%s", request->workspace);
   snprintf(state->last_project, sizeof(state->last_project), "%s", request->project);
   snprintf(state->last_record_type, sizeof(state->last_record_type), "%s", request->record_type);
   if (state->fail)
      return -1;

   reply->request_id = request->request_id;
   reply->generation = request->required_generation;
   /* A reply may never exceed the requested top_k; the route validates it and
    * would reject an over-long one, which is the behaviour under test. */
   reply->count = request->top_k < 2 ? request->top_k : 2;
   reply->candidates[0].point_id = 11;
   reply->candidates[0].score = 0.9;
   if (reply->count > 1)
   {
      reply->candidates[1].point_id = 22;
      reply->candidates[1].score = 0.5;
   }
   return 0;
}

static int allow_all(void *context, const char *workspace, const char *project, int64_t point_id)
{
   (void)context;
   (void)workspace;
   (void)project;
   (void)point_id;
   return 1;
}

static int deny_all(void *context, const char *workspace, const char *project, int64_t point_id)
{
   (void)context;
   (void)workspace;
   (void)project;
   (void)point_id;
   return 0;
}

int main(void)
{
   float vector[4] = {1.0f, 0.0f, 0.0f, 0.0f};
   int64_t ids[8];
   double scores[8];

   /* Without an installed route the adapter reports "no answer" so the caller
    * runs its own pgvector query. It must never look like an empty result. */
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "", 0, ids, scores, 8) == -1);

   search_state_t internal = {0};
   assert(pgvec_db3_route_install(PGVEC_DB3_COLLECTION_MEMORY, fake_search, &internal, allow_all, NULL) == 0);

   /* With no provider selected, the route serves from the internal pgvector
    * callback and the adapter copies the candidates out. */
   int n = pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "", 0, ids, scores, 8);
   assert(n == 2);
   assert(ids[0] == 11 && ids[1] == 22);
   assert(scores[0] > scores[1]);
   assert(internal.calls == 1);
   assert(internal.last_dimension == 4);
   assert(strcmp(internal.last_workspace, "w1") == 0);
   assert(strcmp(internal.last_record_type, "memory") == 0);
   assert(!pgvec_db3_route_serving(PGVEC_DB3_COLLECTION_MEMORY));

   /* Request ids must differ between calls: the wire pairs a reply with its
    * request by this value, so a repeated id could match the wrong reply. */
   uint64_t first_id = internal.last_request_id;
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "", 0, ids, scores, 8) == 2);
   assert(internal.last_request_id != first_id);

   /* An unscoped search cannot be expressed on the wire. Sending one with both
    * components blank would ask a provider to search everything, so it is
    * refused before the route is consulted. */
   int before = internal.calls;
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "", "", 0, ids, scores, 8) == -1);
   assert(internal.calls == before);

   /* limit bounds top_k below max; max bounds it when limit is absent. */
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "", 1, ids, scores, 8) == 1);
   assert(internal.last_top_k == 1);
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "", 0, ids, scores, 3) == 2);
   assert(internal.last_top_k == 3);

   /* A scope component too long for the wire fails the search rather than being
    * truncated: a cut-down workspace name would search a different workspace. */
   char overlong[AIMEE_DB3_MAX_SCOPE + 8];
   memset(overlong, 'w', sizeof(overlong) - 1);
   overlong[sizeof(overlong) - 1] = '\0';
   before = internal.calls;
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", overlong, "", 0, ids, scores, 8) == -1);
   assert(internal.calls == before);

   /* An external provider serves portable reads once selected and ready. */
   search_state_t external = {0};
   assert(pgvec_db3_route_select(PGVEC_DB3_COLLECTION_MEMORY, 456, 1, 0, fake_search, &external) == 0);
   assert(pgvec_db3_route_serving(PGVEC_DB3_COLLECTION_MEMORY));
   before = internal.calls;
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "p1", 0, ids, scores, 8) == 2);
   assert(external.calls == 1);
   assert(internal.calls == before); /* pgvector was not consulted */
   assert(strcmp(external.last_project, "p1") == 0);

   /* With fallback disabled a provider failure surfaces as "no answer" rather
    * than silently returning pgvector's results. */
   external.fail = 1;
   before = internal.calls;
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "", 0, ids, scores, 8) == -1);
   assert(internal.calls == before);

   /* With fallback enabled the same failure falls back to pgvector. */
   assert(pgvec_db3_route_select(PGVEC_DB3_COLLECTION_MEMORY, 456, 1, 1, fake_search, &external) == 0);
   before = internal.calls;
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "", 0, ids, scores, 8) == 2);
   assert(internal.calls == before + 1);

   /* Clearing the provider returns portable reads to pgvector. */
   external.fail = 0;
   pgvec_db3_route_clear(PGVEC_DB3_COLLECTION_MEMORY);
   assert(!pgvec_db3_route_serving(PGVEC_DB3_COLLECTION_MEMORY));
   before = internal.calls;
   int external_before = external.calls;
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "", 0, ids, scores, 8) == 2);
   assert(internal.calls == before + 1);
   assert(external.calls == external_before);

   /* DB2 re-authorizes every candidate. A provider can narrow what DB2
    * considers, never widen it, so a wholly denied reply yields nothing. */
   assert(pgvec_db3_route_install(PGVEC_DB3_COLLECTION_MEMORY, fake_search, &internal, deny_all, NULL) == 0);
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "", 0, ids, scores, 8) <= 0);

   /* Installing a route replaces its callbacks, so any provider selected
    * against the old ones is dropped. */
   assert(pgvec_db3_route_select(PGVEC_DB3_COLLECTION_MEMORY, 456, 1, 0, fake_search, &external) == 0);
   assert(pgvec_db3_route_serving(PGVEC_DB3_COLLECTION_MEMORY));
   assert(pgvec_db3_route_install(PGVEC_DB3_COLLECTION_MEMORY, fake_search, &internal, allow_all, NULL) == 0);
   assert(!pgvec_db3_route_serving(PGVEC_DB3_COLLECTION_MEMORY));

   /* Malformed arguments are refused rather than sent. */
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, NULL, 4, "memory", "w1", "", 0, ids, scores, 8) == -1);
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 0, "memory", "w1", "", 0, ids, scores, 8) == -1);
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, NULL, "w1", "", 0, ids, scores, 8) == -1);
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_MEMORY, vector, 4, "memory", "w1", "", 0, ids, scores, 0) == -1);
   assert(pgvec_db3_route_install(PGVEC_DB3_COLLECTION_MEMORY, NULL, NULL, allow_all, NULL) == -1);
   assert(pgvec_db3_route_install(PGVEC_DB3_COLLECTION_MEMORY, fake_search, NULL, NULL, NULL) == -1);

   /* Routes are per collection, and selecting a provider for one must not make
    * it serve another. A single process-wide route could serve only one
    * collection, so a kb search would have come back with memory candidates
    * that looked like kb answers. */
   search_state_t kb_internal = {0};
   search_state_t kb_external = {0};
   assert(pgvec_db3_route_install(PGVEC_DB3_COLLECTION_KB, fake_search, &kb_internal, allow_all,
                                  NULL) == 0);
   assert(pgvec_db3_route_select(PGVEC_DB3_COLLECTION_KB, 456, 1, 0, fake_search,
                                 &kb_external) == 0);
   assert(pgvec_db3_route_serving(PGVEC_DB3_COLLECTION_KB));
   assert(!pgvec_db3_route_serving(PGVEC_DB3_COLLECTION_MEMORY));

   int memory_internal_before = internal.calls;
   assert(pgvec_db3_candidates(PGVEC_DB3_COLLECTION_KB, vector, 4, "kb", "", "p1", 0, ids, scores,
                               8) == 2);
   assert(kb_external.calls == 1);
   assert(internal.calls == memory_internal_before); /* memory's route untouched */

   /* Clearing one collection leaves the others alone. */
   pgvec_db3_route_clear(PGVEC_DB3_COLLECTION_KB);
   assert(!pgvec_db3_route_serving(PGVEC_DB3_COLLECTION_KB));

   /* A collection this build does not route is refused rather than quietly
    * given a route nothing will ever select a provider for. */
   assert(pgvec_db3_route_install("curator_entity", fake_search, &internal, allow_all, NULL) == -1);
   assert(pgvec_db3_candidates("curator_entity", vector, 4, "", "w1", "", 0, ids, scores, 8) == -1);
   assert(pgvec_db3_candidates(NULL, vector, 4, "", "w1", "", 0, ids, scores, 8) == -1);

   printf("test_pgvec_db3: routing, scope, bounds, collections, and re-authorization passed\n");
   return 0;
}
