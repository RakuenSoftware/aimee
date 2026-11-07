#include "module_commands.h"
#include "json_fluent.h"
#include <assert.h>
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
 *   2. wrong dim: embedder returns != configured width -> op 'failed'.
 *   3. drain batch: multiple pending ops all processed in one drain.
 *
 * (An op for a missing artifact is impossible by construction —
 * evidence_index_ops.artifact_id has an FK to artifacts — so enqueue rejects
 * it; the worker keeps a defensive read-failure guard regardless.)
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "artifacts.h"
#include "evidence_vectors.h"
#include "embed_input_type.h" /* the memory_embed_text stub's polarity argument */
#include "aimee.h"            /* KIND_COUNT, required by memory.h */
#include "memory.h"           /* MEMORY_EMBED_TEST_FIXTURE */
#include "modules/kb/c/kb_store_test_shim.h"
#include "modules/kb/c/kb_store_internal.h"
#include "modules/kb/c/db_postgres.h"
#include "../kb/kb_evidence_embed.h"

/* ---- stub embedder ---------------------------------------------------- */
/* Controls what the stubbed memory_embed_text returns, and records the text it
 * was handed so the test can assert content extraction. */
static int g_embed_dim = 384; /* dims to emit (set per test) */
static int g_embed_calls;
static int g_embed_truncated;
static int g_embed_max_dim;
static char g_embed_last_text[4096];

int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.embed_text") == 0);
   *result = cJSON_CreateObject();
   int max_dim = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(args, "max_dim"));
   g_embed_max_dim = max_dim;
   g_embed_calls++;
   snprintf(g_embed_last_text, sizeof(g_embed_last_text), "%s", jo_cstr(args, "text"));
   int dim = g_embed_dim;
   cJSON_AddNumberToObject(*result, "dim", dim);
   if (g_embed_truncated)
      cJSON_AddBoolToObject(*result, "truncated", 1);
   cJSON *vector = cJSON_AddArrayToObject(*result, "vector");
   for (int i = 0; i < dim; ++i)
      cJSON_AddItemToArray(vector, cJSON_CreateNumber((float)(i % 7) * 0.01f));
   return 1;
}

static void open_db(void)
{
   kb_store_test_shim_close();
   kb_store_test_shim_open();
}

/* Write an evidence artifact and enqueue it for embedding. Returns via id_out. */
static void seed_evidence(char *id_out, size_t id_len, const char *payload)
{
   char id[64];
   kb_store_artifact_gen_id(id, sizeof(id));
   int rc = kb_store_artifact_write(id, "evidence", "proposed", "user", "jbailes", "jbailes", 1.0,
                                    payload);
   assert(rc == 0);
   assert(kb_store_evidence_enqueue(id, "evidence") == 0);
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

   assert(kb_store_evidence_ops_count("pending") == 1);

   int n = kb_evidence_embed_drain(8, MEMORY_EMBED_TEST_FIXTURE);
   assert(n == 1);

   /* Embedder saw the extracted "content", not the raw payload. */
   assert(strcmp(g_embed_last_text, "prefer composition over inheritance") == 0);

   /* store_vector inserts the row AND marks the op 'ok' atomically. */
   assert(kb_store_evidence_ops_count("ok") == 1);
   assert(kb_store_evidence_ops_count("pending") == 0);
   assert(kb_store_evidence_ops_count("failed") == 0);

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

   assert(kb_store_evidence_ops_count("ok") == 0);
   assert(kb_store_evidence_ops_count("failed") == 1);

   /* A failed op is no longer 'pending', so the drain does not spin on it. */
   assert(kb_store_evidence_ops_count("pending") == 0);

   printf("  test_embed_wrong_dim: PASS\n");
}

static void test_embed_truncated(void)
{
   open_db();
   g_embed_dim = 384;
   g_embed_truncated = 1;
   g_embed_calls = 0;

   char id[64];
   seed_evidence(id, sizeof(id), "{\"content\":\"truncated evidence\"}");
   assert(kb_evidence_embed_drain(8, MEMORY_EMBED_TEST_FIXTURE) == 1);
   assert(g_embed_calls == 1);
   /* A full-width vector still fails when the owner reports lost components. */
   assert(kb_store_evidence_ops_count("ok") == 0);
   assert(kb_store_evidence_ops_count("failed") == 1);
   assert(kb_store_evidence_ops_count("pending") == 0);
   g_embed_truncated = 0;
   printf("  test_embed_truncated: PASS\n");
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
   assert(kb_store_evidence_ops_count("pending") == 5);

   int n = kb_evidence_embed_drain(32, MEMORY_EMBED_TEST_FIXTURE);
   assert(n == 5);
   assert(kb_store_evidence_ops_count("ok") == 5);
   assert(kb_store_evidence_ops_count("pending") == 0);

   /* Empty queue: drain reports zero work, not an error. */
   assert(kb_evidence_embed_drain(32, MEMORY_EMBED_TEST_FIXTURE) == 0);

   printf("  test_embed_drain_batch: PASS\n");
}

static void test_embed_configured_width(int configured_dim, int returned_dim)
{
   char width[32];
   snprintf(width, sizeof(width), "%d", configured_dim);
   assert(setenv("EMBEDDER_DIMS", width, 1) == 0);
   open_db();
   if (kb_store_test_shim_is_postgres())
   {
      /* The per-process PostgreSQL clone is seeded at 384 dimensions. The
       * reset above empties its evidence table; size this fixture's column
       * to the instance width before exercising the worker. */
      char sql[160], err[256] = "";
      snprintf(sql, sizeof(sql),
               "ALTER TABLE evidence_vectors ALTER COLUMN embedding TYPE vector(%d)",
               configured_dim);
      assert(aimee_pg_exec(kb_store_conn(), sql, err, sizeof(err)) == 0);
   }
   g_embed_dim = returned_dim;
   g_embed_calls = 0;
   char id[64];
   seed_evidence(id, sizeof(id), "{\"content\":\"configured width evidence\"}");
   assert(kb_evidence_embed_drain(8, MEMORY_EMBED_TEST_FIXTURE) == 1);
   assert(g_embed_calls == 1);
   assert(g_embed_max_dim == configured_dim);
   assert(kb_store_evidence_ops_count("ok") == (configured_dim == returned_dim));
   assert(kb_store_evidence_ops_count("failed") == (configured_dim != returned_dim));
   assert(kb_store_evidence_ops_count("pending") == 0);
}

int main(void)
{
   printf("test_evidence_embed:\n");
   assert(setenv("EMBEDDER_DIMS", "384", 1) == 0);
   test_embed_success();
   test_embed_wrong_dim();
   test_embed_truncated();
   test_embed_drain_batch();
   test_embed_configured_width(768, 768);
   test_embed_configured_width(1024, 1024);
   test_embed_configured_width(768, 384);
   test_embed_configured_width(768, 1024);
   assert(unsetenv("EMBEDDER_DIMS") == 0);
   kb_store_test_shim_close();
   printf("test_evidence_embed: ALL PASS\n");
   return 0;
}
