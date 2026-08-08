/* test_pgvec_neardup.c: LIVE-postgres check of the memory near-duplicate
 * self-join.
 *
 * Why this exists as its own binary: test_pgvec.c deliberately connects to
 * nothing -- it takes the address of each pgvec entry point to prove the symbols
 * link, and calls none of them. That is a linkage test, not a behaviour test, so
 * the near-duplicate SQL in pgvec_memory_vector_near_duplicate_pairs() shipped
 * compiled but never once executed. Its failure mode is silent: a query that
 * errors returns 0 pairs, which is indistinguishable from "nothing was similar",
 * and the read path then suppresses nothing (or, if the cosine sign were
 * inverted, suppresses the WRONG rows) with no error anywhere.
 *
 * So the assertions below go through the real function against a real pgvector
 * table -- prepare, named-parameter binding, step, column reads -- rather than
 * re-typing the SQL into psql, because the binding layer is exactly where a
 * hand-checked query would still have hidden a bug.
 *
 * Skips (exit 0) when AIMEE_TEST_DB2_URL is unset, so CI without a database
 * stays green; the point of the variable is that this can never quietly
 * "pass" by connecting to a deployment an operator did not name. */
#include "db2.h"
#include "lifecycle.h"
#include "memory_vectors.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIM 8

/* Point ids are offset well clear of any real memory id so a mistaken run
 * against a populated database cannot collide with live rows. */
#define BASE_ID 9000000001LL

static void fill(float *v, float x0, float x1)
{
   memset(v, 0, sizeof(float) * DIM);
   v[0] = x0;
   v[1] = x1;
}

static int pair_present(const int64_t *a, const int64_t *b, int n, int64_t want_a, int64_t want_b)
{
   for (int i = 0; i < n; i++)
      if (a[i] == want_a && b[i] == want_b)
         return i;
   return -1;
}

int main(void)
{
   /* Line-buffered: an assert() aborts, and block-buffered stdout would discard
    * the pair dump that says WHY it aborted -- exactly the context needed. */
   setvbuf(stdout, NULL, _IOLBF, 0);

   const char *url = getenv("AIMEE_TEST_DB2_URL");
   if (!url || !*url)
   {
      printf("pgvec_neardup: SKIP (AIMEE_TEST_DB2_URL unset)\n");
      return 0;
   }

   db2_set_embedding_dim(DIM);
   if (db2_init(url) != 0)
   {
      fprintf(stderr, "pgvec_neardup: db2_init failed\n");
      return 1;
   }

   if (pgvec_memory_vector_collection_recreate(DIM) != 0)
   {
      fprintf(stderr, "pgvec_neardup: collection_recreate failed\n");
      return 1;
   }

   /* Four vectors with cosines known by construction, all unit-length in the
    * first two components so the expected values are exact enough to assert on:
    *   1 and 2 are the same direction          -> cosine 1.0
    *   3 is 2 degrees off 1                    -> cosine ~0.9994  (>= 0.94)
    *   4 is orthogonal to 1                    -> cosine 0.0      (<  0.94)
    * Row 5 is never inserted: it stands for a memory written but not yet
    * embedded, and must appear in no pair at all. */
   float v[DIM];
   fill(v, 1.0f, 0.0f);
   assert(pgvec_memory_vector_upsert_memory(BASE_ID + 0, v, DIM, "{}") == 0);
   fill(v, 1.0f, 0.0f);
   assert(pgvec_memory_vector_upsert_memory(BASE_ID + 1, v, DIM, "{}") == 0);
   fill(v, (float)cos(2.0 * M_PI / 180.0), (float)sin(2.0 * M_PI / 180.0));
   assert(pgvec_memory_vector_upsert_memory(BASE_ID + 2, v, DIM, "{}") == 0);
   fill(v, 0.0f, 1.0f);
   assert(pgvec_memory_vector_upsert_memory(BASE_ID + 3, v, DIM, "{}") == 0);

   const int64_t ids[5] = {BASE_ID + 0, BASE_ID + 1, BASE_ID + 2, BASE_ID + 3, BASE_ID + 4};

   int64_t a[16], b[16];
   double cos_out[16];

   /* The production threshold. */
   int n = pgvec_memory_vector_near_duplicate_pairs(ids, 5, 0.94, a, b, cos_out, 16);
   if (n < 0)
   {
      fprintf(stderr, "pgvec_neardup: query returned -1 (no connection?)\n");
      return 1;
   }
   printf("  pairs at 0.94: %d\n", n);
   for (int i = 0; i < n; i++)
      printf("    %lld,%lld cosine=%.6f\n", (long long)a[i], (long long)b[i], cos_out[i]);

   /* The three similar rows pair with each other: (0,1) (0,2) (1,2). */
   assert(n == 3);
   assert(pair_present(a, b, n, BASE_ID + 0, BASE_ID + 1) >= 0);
   assert(pair_present(a, b, n, BASE_ID + 0, BASE_ID + 2) >= 0);
   assert(pair_present(a, b, n, BASE_ID + 1, BASE_ID + 2) >= 0);

   /* The orthogonal row and the unembedded row appear in nothing. */
   for (int i = 0; i < n; i++)
   {
      assert(a[i] != BASE_ID + 3 && b[i] != BASE_ID + 3);
      assert(a[i] != BASE_ID + 4 && b[i] != BASE_ID + 4);
   }

   /* Canonical ordering: a < b, so each unordered pair is reported once. */
   for (int i = 0; i < n; i++)
      assert(a[i] < b[i]);

   /* Cosine is a similarity, not a distance -- identical vectors must score
    * 1.0, not 0.0. This is the assertion that catches a dropped "1.0 -". */
   int idx = pair_present(a, b, n, BASE_ID + 0, BASE_ID + 1);
   assert(idx >= 0);
   assert(cos_out[idx] > 0.999);

   /* Ordered closest-first. */
   for (int i = 1; i < n; i++)
      assert(cos_out[i] <= cos_out[i - 1] + 1e-9);

   /* A threshold above every actual cosine yields nothing -- proves the bound
    * is really applied and the earlier 3 was not "everything, unfiltered". */
   n = pgvec_memory_vector_near_duplicate_pairs(ids, 5, 0.99999, a, b, cos_out, 16);
   printf("  pairs at 0.99999: %d\n", n);
   assert(n == 1); /* only the exactly-identical pair survives */
   assert(pair_present(a, b, n, BASE_ID + 0, BASE_ID + 1) >= 0);

   /* A threshold below everything still excludes nothing but the unembedded
    * row, and picks up the orthogonal one. */
   n = pgvec_memory_vector_near_duplicate_pairs(ids, 5, -1.0, a, b, cos_out, 16);
   printf("  pairs at -1.0: %d\n", n);
   assert(n == 6); /* C(4,2) over the four embedded rows */

   /* max caps the result rather than overflowing the caller's arrays. */
   n = pgvec_memory_vector_near_duplicate_pairs(ids, 5, -1.0, a, b, cos_out, 2);
   printf("  pairs at -1.0 capped to 2: %d\n", n);
   assert(n == 2);

   /* Bad calls are rejected before any SQL runs. */
   assert(pgvec_memory_vector_near_duplicate_pairs(NULL, 5, 0.94, a, b, cos_out, 16) == -1);
   assert(pgvec_memory_vector_near_duplicate_pairs(ids, 1, 0.94, a, b, cos_out, 16) == -1);
   assert(pgvec_memory_vector_near_duplicate_pairs(ids, 5, 0.94, a, b, cos_out, 0) == -1);

   for (int i = 0; i < 4; i++)
      (void)pgvec_memory_vector_delete_point(BASE_ID + i);

   db2_shutdown();
   printf("pgvec_neardup: all tests passed\n");
   return 0;
}
