/* test_evidence_embed.c — unit tests for the evidence-vector embed worker
 * (kb/kb_evidence_embed.c).
 *
 * The worker's job is orchestration: drain evidence_index_ops, read each
 * evidence artifact's content, call the embedder, format the vector, and store
 * it (or mark the op failed). We stub memory_embed_text so the test exercises
 * that orchestration deterministically without linking the memory subsystem or
 * running a real sidecar. The real embedder is covered by the memory tests.
 *
 * Tests:
 *   1. success: pending op -> content embedded -> evidence_vectors row, op 'ok'.
 *   2. wrong dim: embedder returns != 384 -> op 'failed', attempts incremented.
 *   3. drain batch: multiple pending ops all processed in one drain.
 *
 * (An op for a missing artifact is impossible by construction —
 * evidence_index_ops.artifact_id has an FK to artifacts — so enqueue rejects
 * it; the worker keeps a defensive read-failure guard regardless.)
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "artifacts.h"
#include "evidence_vectors.h"
#include "embed_input_type.h" /* the memory_embed_text stub's polarity argument */
#include "aimee.h"            /* KIND_COUNT, required by memory.h */
#include "memory.h"           /* MEMORY_EMBED_TEST_FIXTURE */
#include "modules/db2/c/db2_test_shim.h"
#include "../kb/kb_evidence_embed.h"

/* ---- stub embedder ---------------------------------------------------- */
/* Controls what the stubbed memory_embed_text returns, and records the text it
 * was handed so the test can assert content extraction. */
static int g_embed_dim = 384; /* dims to emit (set per test) */
static int g_embed_calls;
static char g_embed_last_text[4096];

int memory_embed_text(const char *text, const char *command, embed_input_type_t input_type,
                      float *out, int max_dim)
{
   (void)command;
   (void)input_type;
   g_embed_calls++;
   snprintf(g_embed_last_text, sizeof(g_embed_last_text), "%s", text ? text : "");
   int dim = g_embed_dim < max_dim ? g_embed_dim : max_dim;
   for (int i = 0; i < dim; i++)
      out[i] = (float)(i % 7) * 0.01f;
   return dim;
}

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

/* Write an evidence artifact and enqueue it for embedding. Returns via id_out. */
static void seed_evidence(char *id_out, size_t id_len, const char *payload)
{
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   int rc =
       db2_artifact_write(id, "evidence", "proposed", "user", "jbailes", "jbailes", 1.0, payload);
   assert(rc == 0);
   assert(db2_evidence_enqueue(id, "evidence") == 0);
   snprintf(id_out, id_len, "%s", id);
}

/* ---- 1. success ------------------------------------------------------- */
static void test_embed_success(void)
{
   open_db();
   g_embed_dim = 384;
   g_embed_calls = 0;

   char id[64];
   seed_evidence(
       id, sizeof(id),
       "{\"source_kind\":\"feedback\",\"content\":\"prefer composition over inheritance\"}");

   assert(db2_evidence_ops_count("pending") == 1);

   int n = kb_evidence_embed_drain(8, MEMORY_EMBED_TEST_FIXTURE);
   assert(n == 1);

   /* Embedder saw the extracted "content", not the raw payload. */
   assert(strcmp(g_embed_last_text, "prefer composition over inheritance") == 0);

   /* store_vector inserts the row AND marks the op 'ok' atomically. */
   assert(db2_evidence_ops_count("ok") == 1);
   assert(db2_evidence_ops_count("pending") == 0);
   assert(db2_evidence_ops_count("failed") == 0);

   printf("  test_embed_success: PASS\n");
}

/* ---- 2. wrong dim ----------------------------------------------------- */
static void test_embed_wrong_dim(void)
{
   open_db();
   g_embed_dim = 100; /* embedder misbehaves */

   char id[64];
   seed_evidence(id, sizeof(id), "{\"content\":\"short\"}");

   int n = kb_evidence_embed_drain(8, MEMORY_EMBED_TEST_FIXTURE);
   assert(n == 1); /* op was handled (marked failed) */

   assert(db2_evidence_ops_count("ok") == 0);
   assert(db2_evidence_ops_count("failed") == 1);

   /* A failed op is no longer 'pending', so the drain does not spin on it. */
   assert(db2_evidence_ops_count("pending") == 0);

   printf("  test_embed_wrong_dim: PASS\n");
}

/* ---- 3. drain batch --------------------------------------------------- */
static void test_embed_drain_batch(void)
{
   open_db();
   g_embed_dim = 384;

   for (int i = 0; i < 5; i++)
   {
      char id[64];
      char payload[128];
      snprintf(payload, sizeof(payload), "{\"content\":\"evidence number %d\"}", i);
      seed_evidence(id, sizeof(id), payload);
   }
   assert(db2_evidence_ops_count("pending") == 5);

   int n = kb_evidence_embed_drain(32, MEMORY_EMBED_TEST_FIXTURE);
   assert(n == 5);
   assert(db2_evidence_ops_count("ok") == 5);
   assert(db2_evidence_ops_count("pending") == 0);

   /* Empty queue: drain reports zero work, not an error. */
   assert(kb_evidence_embed_drain(32, MEMORY_EMBED_TEST_FIXTURE) == 0);

   printf("  test_embed_drain_batch: PASS\n");
}

int main(void)
{
   printf("test_evidence_embed:\n");
   test_embed_success();
   test_embed_wrong_dim();
   test_embed_drain_batch();
   db2_test_shim_close();
   printf("test_evidence_embed: ALL PASS\n");
   return 0;
}
