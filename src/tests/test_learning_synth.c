/* test_learning_synth.c — unit tests for the candidate-generation pass
 * (kb/kb_learning_synth.c).
 *
 * We stub memory_embed_text (so the neighbourhood bundle is deterministic) and
 * stub the model sidecar with a `cat <canned-file>` command, so the worker's
 * orchestration — build bundle, call sidecar, write proposed candidates, cite
 * the neighbourhood — is exercised end-to-end over the db2 sqlite shim without
 * a live LLM.
 *
 * Tests:
 *   1. success: a candidate is written proposed, citing every neighbour.
 *   2. sidecar error status -> -1, nothing written.
 *   3. empty corpus -> 0, sidecar never invoked.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "artifacts.h"
#include "evidence_vectors.h"
#include "learning_synth_ops.h"
#include "embed_input_type.h" /* the memory_embed_text stub's polarity argument */
#include "modules/db2/c/db2_test_shim.h"
#include "../kb/kb_learning_synth.h"

#define RESP_PATH "/tmp/aimee_synth_test_resp.json"
#define STUB_CMD  "cat " RESP_PATH

/* Deterministic embedder: query vector is e0 so the bundle ranks/spans fine. */
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

static void write_resp(const char *json)
{
   FILE *f = fopen(RESP_PATH, "w");
   assert(f);
   fputs(json, f);
   fclose(f);
}

static void make_vec384(char *buf, size_t n, float v0)
{
   size_t pos = 0;
   pos += (size_t)snprintf(buf + pos, n - pos, "[%.6f", v0);
   for (int i = 1; i < 384; i++)
      pos += (size_t)snprintf(buf + pos, n - pos, ",0.000000");
   snprintf(buf + pos, n - pos, "]");
}

static void seed_id(const char *kind, const char *content, float v0, char *id_out, size_t id_len)
{
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   char payload[256];
   snprintf(payload, sizeof(payload), "{\"content\":\"%s\"}", content);
   assert(db2_artifact_write(id, kind, "proposed", "user", "jbailes", "jbailes", 1.0, payload) ==
          0);
   assert(db2_evidence_enqueue(id, "evidence") == 0);
   char vec[8192];
   make_vec384(vec, sizeof(vec), v0);
   assert(db2_evidence_store_vector(id, "evidence", vec) == 0);
   if (id_out)
      snprintf(id_out, id_len, "%s", id);
}

static void seed(const char *kind, const char *content, float v0)
{
   seed_id(kind, content, v0, NULL, 0);
}

/* ---- 1. success ------------------------------------------------------- */
static void test_synth_success(void)
{
   open_db();
   write_resp("{\"version\":1,\"status\":\"ok\",\"candidates\":["
              "{\"kind\":\"anti_pattern\",\"payload\":{\"title\":\"t\",\"content\":\"c\"},"
              "\"confidence\":0.9}]}");

   seed("feedback_negative", "do not retry on auth failure", 1.0f);
   seed("guardrail_event", "auth retry blocked", 0.9f);
   seed("session_turn", "user hit auth retry loop", 0.8f);

   char ids[8][37];
   int n = kb_learning_synth_generate("auth retry", STUB_CMD, "stub", 8, 2048, "user", "jbailes",
                                      "jbailes", ids, 8);
   assert(n == 1);

   /* The candidate is a proposed anti_pattern... */
   db2_artifact_row_t row;
   assert(db2_artifact_read(ids[0], &row, NULL, 0, NULL) == 0);
   assert(strcmp(row.kind, "anti_pattern") == 0);
   assert(strcmp(row.state, "proposed") == 0);

   /* ...citing every neighbourhood evidence artifact (corroboration = 3). */
   assert(db2_artifact_citation_count(ids[0]) == 3);

   printf("  test_synth_success: PASS\n");
}

/* ---- 2. sidecar error ------------------------------------------------- */
static void test_synth_sidecar_error(void)
{
   open_db();
   write_resp("{\"version\":1,\"status\":\"error\",\"error\":\"llm down\"}");

   seed("feedback_negative", "x", 1.0f);
   seed("guardrail_event", "y", 0.9f);

   int n = kb_learning_synth_generate("q", STUB_CMD, "stub", 8, 2048, "user", "jbailes", "jbailes",
                                      NULL, 0);
   assert(n == -1);
   /* No candidate artifacts beyond the 2 seeded evidence rows. */
   assert(db2_artifact_count("anti_pattern", NULL) == 0);

   printf("  test_synth_sidecar_error: PASS\n");
}

/* ---- 3. empty corpus -------------------------------------------------- */
static void test_synth_empty_corpus(void)
{
   open_db();
   write_resp("{\"version\":1,\"status\":\"ok\",\"candidates\":[]}");

   /* No evidence vectors at all -> empty bundle -> 0, sidecar not consulted. */
   int n = kb_learning_synth_generate("q", STUB_CMD, "stub", 8, 2048, "user", "jbailes", "jbailes",
                                      NULL, 0);
   assert(n == 0);

   printf("  test_synth_empty_corpus: PASS\n");
}

/* ---- 4. scheduler drain ----------------------------------------------- */
static void test_synth_drain(void)
{
   open_db();
   write_resp("{\"version\":1,\"status\":\"ok\",\"candidates\":["
              "{\"kind\":\"anti_pattern\",\"payload\":{\"title\":\"t\"},\"confidence\":0.8}]}");

   char id1[64], id2[64];
   seed_id("feedback_negative", "retry on auth failure", 1.0f, id1, sizeof(id1));
   seed_id("guardrail_event", "auth retry blocked", 0.9f, id2, sizeof(id2));
   assert(db2_synth_enqueue(id1) == 0);
   assert(db2_synth_enqueue(id2) == 0);
   assert(db2_synth_ops_count("pending") == 2);

   int n = kb_learning_synth_drain(16, STUB_CMD, "stub", 8, 2048);
   assert(n == 2); /* both ops processed */
   assert(db2_synth_ops_count("ok") == 2);
   assert(db2_synth_ops_count("pending") == 0);
   /* One candidate per processed op. */
   assert(db2_artifact_count("anti_pattern", "proposed") == 2);

   /* Drained queue is a no-op. */
   assert(kb_learning_synth_drain(16, STUB_CMD, "stub", 8, 2048) == 0);

   printf("  test_synth_drain: PASS\n");
}

int main(void)
{
   printf("test_learning_synth:\n");
   test_synth_success();
   test_synth_sidecar_error();
   test_synth_empty_corpus();
   test_synth_drain();
   db2_test_shim_close();
   remove(RESP_PATH);
   printf("test_learning_synth: ALL PASS\n");
   return 0;
}
