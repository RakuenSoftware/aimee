/* test_memory_embed_dim_guard.c: the memory/KB vector upsert must reject a
 * vector whose dimension does not match the configured embedding dim
 * (db2_embedding_dim), instead of silently "succeeding" while Postgres rejects
 * the row ("expected N dimensions, not M") and memory_embeddings stays empty.
 *
 * This reproduces the .254 failure where the reembed path fell back to the
 * builtin 384-dim embedder against a vector(1024) column: every insert failed
 * but was counted as a successful embed. Covered for both shipping dims:
 *   1024 -> pplx-embed-v1-0.6b
 *   2560 -> pplx-embed-v1-4b
 *
 * Exercised against the in-memory sqlite shim, and against real Postgres at the
 * width the template's column actually has (see main). */
#include "../headers/aimee.h"
#include "../headers/config_embedder_dims.h"
#include "../modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/db_postgres.h"    /* aimee_pg_is_shim */
#include "../modules/db2/c/lifecycle.h"      /* db2_set_embedding_dim */
#include "../modules/db2/c/memory_vectors.h" /* pgvec_memory_vector_upsert_memory + search */
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

   if (aimee_pg_is_shim())
   {
      /* The shim's embedding column is untyped text, so the configured dim can be
       * moved freely and every shipping width is reachable. */
      check_dim(1024, 384);  /* pplx-0.6b column; the builtin 384-dim vector must be rejected */
      check_dim(2560, 1024); /* pplx-4b column; a 1024-dim (0.6b) vector must be rejected */
      check_dim(2560, 384);  /* pplx-4b column; the builtin 384-dim vector must be rejected too */
   }
   else
   {
      /* Against real Postgres the column is halfvec(N), fixed when the test template
       * was built, and db2_set_embedding_dim only moves the in-memory guard -- it does
       * not re-type the column. Widths other than the template's are therefore
       * unreachable here. Exercise the guard at the width the column really has: that
       * is the half of this test a schemaless backend cannot verify at all, because
       * only a real halfvec column can confirm the matching vector actually persisted
       * rather than being stored as text nobody would ever read back as a vector. */
      check_dim(CONFIG_EMBEDDER_DIMS_DEFAULT, CONFIG_EMBEDDER_DIMS_DEFAULT * 2);
   }

   printf("test_memory_embed_dim_guard: OK\n");
   return 0;
}
