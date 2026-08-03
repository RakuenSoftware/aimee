/* test_learning_bundle.c — unit tests for the cross-source neighbourhood
 * builder (learning_bundle.c).
 *
 * We stub memory_embed_text to return a fixed query vector and store crafted
 * evidence vectors directly, so cosine ordering is fully deterministic and the
 * test is independent of any real embedder. The point under test is the
 * ranking + cross-kind spanning, over the db2 sqlite shim.
 *
 * Tests:
 *   1. ranks by cosine, spans >= 3 kinds, orders nearest-first.
 *   2. k clamps the result size.
 *   3. empty corpus yields an empty (not error) bundle.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "artifacts.h"
#include "evidence_vectors.h"
#include "embed_input_type.h" /* the memory_embed_text stub's polarity argument */
#include "db2_test_shim.h"
#include "modules/learning/learning_bundle.h"

/* Stub embedder: query vector is the unit basis e0 = [1,0,0,...]. Cosine with a
 * stored [v0,v1,0,...] is then v0 / sqrt(v0^2 + v1^2), so we can hand-pick the
 * ordering by choosing each evidence vector's first two components. */
int memory_embed_text(const char *text, const char *command, embed_input_type_t input_type,
                      float *out, int max_dim)
{
   (void)text;
   (void)command;
   (void)input_type;
   int dim = 384 < max_dim ? 384 : max_dim;
   for (int i = 0; i < dim; i++)
      out[i] = 0.0f;
   out[0] = 1.0f;
   return dim;
}

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

/* Build a 384-dim pgvector text literal "[v0,v1,0,...,0]". */
static void make_vec384(char *buf, size_t n, float v0, float v1)
{
   size_t pos = 0;
   pos += (size_t)snprintf(buf + pos, n - pos, "[%.6f,%.6f", v0, v1);
   for (int i = 2; i < 384; i++)
      pos += (size_t)snprintf(buf + pos, n - pos, ",0.000000");
   snprintf(buf + pos, n - pos, "]");
}

/* Write an evidence artifact of `kind` and store its vector. */
static void seed(const char *kind, float v0, float v1)
{
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   assert(db2_artifact_write(id, kind, "proposed", "user", "jbailes", "jbailes", 1.0, "{}") == 0);
   assert(db2_evidence_enqueue(id, "evidence") == 0);
   char vec[8192];
   make_vec384(vec, sizeof(vec), v0, v1);
   assert(db2_evidence_store_vector(id, "evidence", vec) == 0);
}

/* ---- 1. cross-kind ranking ------------------------------------------- */
static void test_bundle_ranks_across_kinds(void)
{
   open_db();

   /* Three kinds, decreasing similarity to e0: */
   seed("feedback_negative", 1.0f, 0.0f); /* cos = 1.000 */
   seed("guardrail_event", 1.0f, 1.0f);   /* cos = 0.707 */
   seed("session_turn", 0.0f, 1.0f);      /* cos = 0.000 */

   learning_bundle_t b;
   int rc = learning_bundle_build("any query", "stub", 8, &b);
   assert(rc == 0);

   assert(b.scanned == 3);
   assert(b.count == 3);
   assert(b.distinct_kinds == 3); /* the >= 3 kinds the AC requires */

   /* Nearest-first ordering. */
   assert(strcmp(b.items[0].kind, "feedback_negative") == 0);
   assert(strcmp(b.items[2].kind, "session_turn") == 0);
   assert(b.items[0].score >= b.items[1].score);
   assert(b.items[1].score >= b.items[2].score);
   assert(b.items[0].score > 0.99); /* ~1.0 */
   assert(b.items[2].score < 0.01); /* ~0.0 */

   printf("  test_bundle_ranks_across_kinds: PASS\n");
}

/* ---- 2. k clamps result size ----------------------------------------- */
static void test_bundle_k_limit(void)
{
   open_db();
   seed("feedback_negative", 1.0f, 0.0f);
   seed("guardrail_event", 0.9f, 0.1f);
   seed("session_turn", 0.5f, 0.5f);

   learning_bundle_t b;
   assert(learning_bundle_build("q", "stub", 2, &b) == 0);
   assert(b.scanned == 3);
   assert(b.count == 2); /* clamped to k */
   assert(b.items[0].score >= b.items[1].score);

   printf("  test_bundle_k_limit: PASS\n");
}

/* ---- 3. empty corpus -------------------------------------------------- */
static void test_bundle_empty(void)
{
   open_db();

   learning_bundle_t b;
   int rc = learning_bundle_build("q", "stub", 5, &b);
   assert(rc == 0); /* not an error */
   assert(b.scanned == 0);
   assert(b.count == 0);
   assert(b.distinct_kinds == 0);

   printf("  test_bundle_empty: PASS\n");
}

int main(void)
{
   printf("test_learning_bundle:\n");
   test_bundle_ranks_across_kinds();
   test_bundle_k_limit();
   test_bundle_empty();
   db2_test_shim_close();
   printf("test_learning_bundle: ALL PASS\n");
   return 0;
}
