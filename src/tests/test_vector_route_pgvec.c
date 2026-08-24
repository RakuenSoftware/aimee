/* test_vector_route_pgvec.c: LIVE-postgres check that the memory vector search
 * still answers correctly now that it goes through the vector route.
 *
 * Why this exists as its own binary. Wiring
 * `pgvec_memory_vector_search_record_type` and `..._with_kinds` through
 * `aimee_vector_memory_candidates_search` inserted a request codec, a validator,
 * a route decision and a result copy between every semantic memory query and the
 * SQL that answers it. Each of those can fail in a way that returns FEWER rows,
 * or none, rather than an error -- and memory search reads "no vector results"
 * as "nothing was similar", which is a plausible answer. A regression here does
 * not crash and does not log; it quietly makes recall worse.
 *
 * The unit suite (unit-test-vector-route) proves the route's own logic against
 * fakes, and unit-test-pgvec proves the symbols link while calling none of them.
 * Neither one would notice if the request the wrapper builds failed validation,
 * because neither one builds a request. That is precisely the bug this file was
 * written after finding: `aimee_vector_search_request_validate` refuses
 * request_id 0, required_generation 0, and a request with no scope at all, and a
 * memset-and-fill supplied all three.
 *
 * The load-bearing assertion is the COMPARISON: every routed call is checked
 * against the same query issued directly, on the same data, in the same scope.
 * An assertion on a fixed expected count would pass against a route that had
 * quietly stopped routing.
 *
 * Skips (exit 0) when AIMEE_TEST_DB2_URL is unset, so CI without a database
 * stays green; the point of the variable is that this can never quietly "pass"
 * by connecting to a deployment an operator did not name. */
#include "modules/db2/c/db2.h"
#include "lifecycle.h"
#include "memory_scope_query.h"
#include "memory_vectors.h"
#include "pgvec_transport.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The width the database records, read back after init: the schema's dim wins
 * over a caller's request, which is what the upsert dim guard exists to
 * enforce. Filled in main() before any vector is built. */
static int DIM;

/* Clear of any real memory id, so a mistaken run against a populated database
 * cannot collide with live rows. */
#define BASE_ID 9100000001LL

#define WS "route-ws"
#define PJ "route-pj"

/* The scope the fixture rows carry. Without it the column filter excludes every
 * one of them and the comparison below compares nothing to nothing -- which is
 * what the first live run did, and what the `n > 0` assertion caught. */
/* record_type and kind are columns the search filters on, and the upsert reads
 * both from the payload. Omitting them left the rows with empty strings, so
 * `WHERE record_type = 'memory'` matched nothing -- the first live run compared
 * an empty result against an empty result and would have "passed" without the
 * `n > 0` assertion. */
#define PAYLOAD(k)                                                                                 \
   "{\"record_type\":\"memory\",\"kind\":\"" k "\",\"workspace\":\"" WS "\",\"project\":\"" PJ "\"}"

/* Two non-zero components in a vector of the schema's width: the cosines are
 * then exact enough to assert an ORDER on, which is the property the route
 * could silently destroy. */
static void fill(float *v, float x0, float x1)
{
   memset(v, 0, sizeof(float) * (size_t)DIM);
   v[0] = x0;
   v[1] = x1;
}

/* Same query, once through the route and once straight at the SQL. Returns the
 * routed count; aborts if the two disagree in count, order or score. */
static int compare(const char *label, const char *record_type, const float *vec,
                   const char *const *kinds, int n_kinds, int limit, int max)
{
   int64_t routed_ids[64], direct_ids[64];
   double routed_scores[64], direct_scores[64];

   int routed = n_kinds > 0 ? pgvec_memory_vector_search_with_kinds(vec, DIM, kinds, n_kinds, limit,
                                                                    routed_ids, routed_scores, max)
                            : pgvec_memory_vector_search_record_type(
                                  record_type, vec, DIM, limit, routed_ids, routed_scores, max);

   /* The direct call reads the same thread-local scope context, so this is the
    * query the wrapper would have issued before the route existed. */
   int direct = pgvec_memory_search(vec, DIM, n_kinds > 0 ? "memory" : record_type, kinds, n_kinds,
                                    WS, PJ, limit, direct_ids, direct_scores, max);

   printf("  %-28s routed=%d direct=%d\n", label, routed, direct);
   assert(routed == direct);
   for (int i = 0; i < routed; i++)
   {
      assert(routed_ids[i] == direct_ids[i]);
      assert(fabs(routed_scores[i] - direct_scores[i]) < 1e-9);
   }
   return routed;
}

int main(void)
{
   /* Line-buffered: an assert() aborts, and block-buffered stdout would discard
    * the counts that say WHICH comparison failed. */
   setvbuf(stdout, NULL, _IOLBF, 0);

   const char *url = getenv("AIMEE_TEST_DB2_URL");
   if (!url || !*url)
   {
      printf("vector_route_pgvec: SKIP (AIMEE_TEST_DB2_URL unset)\n");
      return 0;
   }

   if (db2_init(url) != 0)
   {
      fprintf(stderr, "vector_route_pgvec: db2_init failed\n");
      return 1;
   }
   /* Not a recreate. memory_embeddings is the table the vector projection
    * captures from, and truncating it is refused for good reason; this test
    * adds its own points and removes them below. */
   DIM = db2_embedding_dim();
   if (DIM <= 0 || DIM > 4096)
   {
      fprintf(stderr, "vector_route_pgvec: implausible embedding dim %d\n", DIM);
      return 1;
   }
   printf("  embedding dim: %d\n", DIM);

   /* Four points along two directions, so a nearest-neighbour query has a
    * non-trivial order to get wrong. */
   float *v = calloc((size_t)DIM, sizeof(float));
   float *query = calloc((size_t)DIM, sizeof(float));
   assert(v && query);
   fill(v, 1.0f, 0.0f);
   assert(pgvec_memory_vector_upsert_memory(BASE_ID + 0, v, DIM, PAYLOAD("fact")) == 0);
   fill(v, (float)cos(2.0 * M_PI / 180.0), (float)sin(2.0 * M_PI / 180.0));
   assert(pgvec_memory_vector_upsert_memory(BASE_ID + 1, v, DIM, PAYLOAD("fact")) == 0);
   fill(v, (float)cos(30.0 * M_PI / 180.0), (float)sin(30.0 * M_PI / 180.0));
   assert(pgvec_memory_vector_upsert_memory(BASE_ID + 2, v, DIM, PAYLOAD("lesson")) == 0);
   fill(v, 0.0f, 1.0f);
   assert(pgvec_memory_vector_upsert_memory(BASE_ID + 3, v, DIM, PAYLOAD("lesson")) == 0);

   fill(query, 1.0f, 0.0f);

   /* ---------------------------------------------------------------- routed
    *
    * A scope hint is what makes the request expressible: without one there is
    * no tenancy statement to carry, and the wrapper deliberately takes the
    * direct path. So the hint is not decoration here -- it is the precondition
    * for this file testing the thing it claims to test, and the assertion below
    * proves the routed path really was taken. */
   pgvec_memory_vector_scope_hint_set(WS, PJ);

   /* The counter is what makes this file test the route rather than merely
    * exercise the memory search. Both paths return identical results by design,
    * so without it every assertion below would hold just as well against a
    * wrapper that refused every request and went straight to SQL. */
   uint64_t routed_before = pgvec_memory_vector_routed_searches();

   int n = compare("record_type, no kinds", "memory", query, NULL, 0, 4, 64);
   assert(n > 0 && "no rows came back at all -- the comparison proves nothing");
   /* Two searches per compare(): the routed one and the direct one, of which
    * exactly the first goes through the route. */
   assert(pgvec_memory_vector_routed_searches() == routed_before + 1);
   printf("  routed (counter moved)\n");

   /* Nearest first: the identical direction, then 2 degrees, then 30. The
    * order is the part a broken result copy silently destroys. */
   int64_t ids[64];
   double scores[64];
   assert(pgvec_memory_vector_search_record_type("memory", query, DIM, 3, ids, scores, 64) == 3);
   assert(ids[0] == BASE_ID + 0 && ids[1] == BASE_ID + 1 && ids[2] == BASE_ID + 2);
   assert(scores[0] >= scores[1] && scores[1] >= scores[2]);
   printf("  order preserved through the route\n");

   /* The kind restriction is the reason search version 2 exists: version 1
    * could not express it, so this call could not have been routed at all. */
   const char *kinds[] = {"fact", "lesson"};
   compare("with kinds (2)", NULL, query, kinds, 2, 4, 64);
   const char *one_kind[] = {"fact"};
   compare("with kinds (1)", NULL, query, one_kind, 1, 4, 64);

   /* top_k above the contract's ceiling is not expressible and takes the direct
    * path. It must still answer, and answer identically. */
   uint64_t before_oversized = pgvec_memory_vector_routed_searches();
   compare("limit above contract max", "memory", query, NULL, 0, 512, 64);
   /* 512 is above the contract's top_k ceiling, so this one is NOT expressible
    * and must take the direct path. Asserting the counter did not move is what
    * keeps the fallback from being invisible: without it, a build_request that
    * refused everything would look exactly like a working route. */
   assert(pgvec_memory_vector_routed_searches() == before_oversized);
   printf("  oversized limit bypassed the route, as it must\n");

   /* ------------------------------------------------------- no scope at all
    *
    * Cleared hints mean no tenancy statement, so this is the direct path by
    * construction. It is here because that path must keep working: it is what
    * every legacy direct caller in the tree still uses. */
   pgvec_memory_vector_scope_hint_clear();
   uint64_t before_unscoped = pgvec_memory_vector_routed_searches();
   int unscoped = pgvec_memory_vector_search_record_type("memory", query, DIM, 4, ids, scores, 64);
   assert(pgvec_memory_vector_routed_searches() == before_unscoped);
   printf("  unscoped (direct path)      rows=%d\n", unscoped);
   assert(unscoped >= 0);

   /* ------------------------------------------------------- authorisation
    *
    * The callback that guards results from an external provider. With no scope
    * context active every point is visible, which is the same answer the search
    * gives an unscoped caller -- the two must agree, because they are the same
    * clause. */
   db2_memory_scope_context_clear();
   assert(pgvec_memory_point_visible(BASE_ID + 0) == 1);
   /* A point the provider invented does not exist, so it is refused. This is
    * also the staleness check: an id deleted since the provider indexed it
    * fails the same equality. */
   assert(pgvec_memory_point_visible(BASE_ID + 9999) == 0);
   printf("  visibility: real point admitted, invented point refused\n");

   /* A shared CI database: points left behind would show up in every later
    * query as plausible neighbours. */
   for (int i = 0; i < 4; i++)
      assert(pgvec_memory_vector_delete_point(BASE_ID + i) == 0);
   free(v);
   free(query);

   printf("test_vector_route_pgvec: OK\n");
   return 0;
}
