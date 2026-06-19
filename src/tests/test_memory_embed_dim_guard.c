/* test_memory_embed_dim_guard.c: the memory/KB vector upsert must reject a
 * vector whose dimension does not match the configured embedding dim
 * (db2_embedding_dim), instead of silently "succeeding" while Postgres rejects
 * the row ("expected N dimensions, not M") and memory_embeddings stays empty.
 *
 * This reproduces the .254 failure where the reembed path fell back to the
 * builtin 384-dim embedder against a halfvec(1024) column: every insert failed
 * but was counted as a successful embed. Covered for both shipping dims:
 *   1024 -> pplx-embed-v1-0.6b
 *   2560 -> pplx-embed-v1-4b
 *
 * Exercised against the in-memory sqlite shim. */
#include "../headers/aimee.h"
#include "../db2/db2_test_shim.h"
#include "../db2/lifecycle.h"      /* db2_set_embedding_dim */
#include "../db2/memory_vectors.h" /* pgvec_memory_vector_upsert_memory + search */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static float *make_vec(int dim)
{
   float *v = calloc((size_t)dim, sizeof(float));
   assert(v);
   for (int i = 0; i < dim; i++)
      v[i] = 0.01f * (float)((i % 7) + 1);
   return v;
}

/* For a configured dim `expect`, a matching-dim vector persists and is
 * retrievable; a `wrong`-dim vector is rejected (non-zero) and never stored. */
static void check_dim(int expect, int wrong)
{
   db2_set_embedding_dim(expect);

   /* Matching dim: upsert succeeds. */
   float *ok = make_vec(expect);
   int64_t mid = 100000 + expect;
   int rc = pgvec_memory_vector_upsert_memory(mid, ok, expect, "{\"record_type\":\"memory\"}");
   assert(rc == 0);

   /* It actually persisted: a search by the same vector finds the point. */
   int64_t ids[16];
   double scores[16];
   int n = pgvec_memory_vector_search_record_type("memory", ok, expect, 16, ids, scores, 16);
   int found = 0;
   for (int i = 0; i < n; i++)
      if (ids[i] == mid)
         found = 1;
   assert(found && "matching-dim embedding should persist and be retrievable");
   free(ok);

   /* Wrong dim (e.g. builtin 384 vs a 1024/2560 column): rejected, NOT counted
    * as a successful embed. */
   float *bad = make_vec(wrong);
   int rc2 = pgvec_memory_vector_upsert_memory(mid + 1, bad, wrong, "{\"record_type\":\"memory\"}");
   assert(rc2 != 0 && "dim-mismatched embedding must be rejected, not silently succeed");
   free(bad);

   printf("  dim guard ok: expect=%d persists, wrong=%d rejected\n", expect, wrong);
}

int main(void)
{
   db2_test_shim_open();

   check_dim(1024, 384);  /* pplx-0.6b column; the builtin 384-dim vector must be rejected */
   check_dim(2560, 1024); /* pplx-4b column; a 1024-dim vector must be rejected */

   printf("test_memory_embed_dim_guard: OK\n");
   return 0;
}
