#include "module_commands.h"
#include "kb_curator_llm.h"
#include <stdlib.h>
#include "json_fluent.h"
#include <assert.h>
/* test_learning_synth.c — unit tests for the candidate-generation pass
 * (kb/kb_learning_synth.c).
 *
 * We stub memory_embed_text (so the neighbourhood bundle is deterministic) and
 * stub the canonical provider with owned typed IR responses, so the worker's
 * orchestration — build bundle, call canonical provider, write proposed candidates, cite
 * the neighbourhood — is exercised end-to-end over the kb_store sqlite shim without
 * a live LLM.
 *
 * Tests:
 *   1. success: a candidate is written proposed, citing every neighbour.
 *   2. failed business result -> -1, nothing written.
 *   3. empty corpus -> 0, provider never invoked.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "artifacts.h"
#include "evidence_vectors.h"
#include "learning_synth_ops.h"
#include "embed_input_type.h" /* the memory_embed_text stub's polarity argument */
#include "modules/kb/c/kb_store_test_shim.h"
#include "../kb/kb_learning_synth.h"

#define STUB_CMD "retired-command-must-never-execute"
static char *model_text;
static int model_mode, model_calls;

int kb_curator_error_is_provider_unavailable(const char *error)
{
   (void)error;
   return 0;
}
void kb_curator_provider_backoff_note(void)
{
}
void kb_curator_provider_backoff_recovered(void)
{
}
int kb_curator_provider_for_stage(kb_curator_stage_t stage, provider_def_owned_t *out)
{
   assert(stage == KB_CURATOR_STAGE_SYNTHESIZE);
   memset(out, 0, sizeof *out);
   out->def.base_url = "http://unused.invalid";
   out->def.model = "canonical-fixture";
   return 1;
}
int provider_client_complete_ir(const provider_def_t *def, const aimee_request_t *request,
                                provider_completion_t *out, char *error, size_t error_len)
{
   (void)error;
   (void)error_len;
   model_calls++;
   assert(!aimee_ir_request_admit(request));
   assert(request->n_messages == 1 && !strcmp(request->messages[0].role, "user"));
   assert(request->n_system == 1 && request->system[0].type == AIMEE_BLK_TEXT);
   assert(strstr(request->system[0].text, "at least two"));
   assert(strstr(request->system[0].text,
                 "preference, workflow, anti_pattern, mistake_pattern, synthesis"));
   assert(request->has_max_tokens && request->max_tokens == 2048 && def->max_tokens == 0);
   assert(request->n_tools == 0);
   cJSON *job = cJSON_Parse(request->messages[0].blocks[0].text);
   assert(job && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(job, "role")),
                         "synthesize"));
   cJSON *input = cJSON_GetObjectItemCaseSensitive(job, "input");
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(input, "query")));
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(input, "neighbours")) >= 1);
   cJSON_Delete(job);
   aimee_block_t blocks[2] = {{.type = AIMEE_BLK_THINKING,
                               .text = "{\"candidates\":[{\"kind\":\"preference\",\"payload\":{"
                                       "\"content\":\"private reasoning must not be written\"}}]}"},
                              {.type = AIMEE_BLK_TEXT, .text = model_text}};
   aimee_refusal_details_t refusal = {.category = "future_category", .explanation = "diagnostic"};
   aimee_response_t response = {
       .role = "assistant", .content = blocks, .n_content = 2, .stop_reason = AIMEE_STOP_END_TURN};
   if (model_mode == 1)
   {
      response.stop_reason = AIMEE_STOP_CONTENT_FILTER;
      response.raw_stop_reason = "refusal";
      response.refusal = &refusal;
   }
   if (model_mode == 2)
   {
      response.stop_reason = AIMEE_STOP_ERROR;
      response.error_message = "provider failed";
   }
   if (model_mode == 3)
      response.n_content = 1;
   if (model_mode == 4)
      response.stop_reason = AIMEE_STOP_MAX_TOKENS;
   assert(!aimee_ir_response_clone(&response, &out->ir));
   return 0;
}
void provider_completion_free(provider_completion_t *out)
{
   aimee_response_free(&out->ir);
   memset(out, 0, sizeof *out);
}

/* Deterministic embedder: query vector is e0 so the bundle ranks/spans fine. */
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.embed_text") == 0);
   *result = cJSON_CreateObject();
   int max_dim = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(args, "max_dim"));
   int dim = max_dim < 384 ? max_dim : 384;
   cJSON *vector = cJSON_AddArrayToObject(*result, "vector");
   for (int i = 0; i < dim; ++i)
      cJSON_AddItemToArray(vector, cJSON_CreateNumber(i == 0 ? 1.0f : 0.0f));
   return 1;
}

static void open_db(void)
{
   kb_store_test_shim_close();
   kb_store_test_shim_open();
   model_mode = 0;
   model_calls = 0;
}

static void write_resp(const char *json)
{
   free(model_text);
   model_text = strdup(json);
   assert(model_text);
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
   kb_store_artifact_gen_id(id, sizeof(id));
   char payload[256];
   snprintf(payload, sizeof(payload), "{\"content\":\"%s\"}", content);
   assert(kb_store_artifact_write(id, kind, "proposed", "user", "jbailes", "jbailes", 1.0,
                                  payload) == 0);
   assert(kb_store_evidence_enqueue(id, "evidence") == 0);
   char vec[8192];
   make_vec384(vec, sizeof(vec), v0);
   assert(kb_store_evidence_store_vector(id, "evidence", vec) == 0);
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
   int n = kb_learning_synth_generate("auth retry", NULL, "stub", 8, 2048, "user", "jbailes",
                                      "jbailes", ids, 8);
   assert(n == 1);

   /* The candidate is a proposed anti_pattern... */
   kb_store_artifact_row_t row;
   assert(kb_store_artifact_read(ids[0], &row, NULL, 0, NULL) == 0);
   assert(strcmp(row.kind, "anti_pattern") == 0);
   assert(strcmp(row.state, "proposed") == 0);

   /* ...citing every neighbourhood evidence artifact (corroboration = 3). */
   assert(kb_store_artifact_citation_count(ids[0]) == 3);
   assert(kb_store_artifact_count("preference", NULL) == 0);

   printf("  test_synth_success: PASS\n");
}

/* ---- 2. failed business result ------------------------------------------------- */
static void test_synth_failed_business_result(void)
{
   open_db();
   write_resp("{\"version\":1,\"status\":\"error\",\"error\":\"llm down\"}");

   seed("feedback_negative", "x", 1.0f);
   seed("guardrail_event", "y", 0.9f);

   int n = kb_learning_synth_generate("q", NULL, "stub", 8, 2048, "user", "jbailes", "jbailes",
                                      NULL, 0);
   assert(n == -1);
   /* No candidate artifacts beyond the 2 seeded evidence rows. */
   assert(kb_store_artifact_count("anti_pattern", NULL) == 0);

   printf("  test_synth_failed_business_result: PASS\n");
}

/* ---- 3. empty corpus -------------------------------------------------- */
static void test_synth_empty_corpus(void)
{
   open_db();
   write_resp("{\"version\":1,\"status\":\"ok\",\"candidates\":[]}");

   /* No evidence vectors at all -> empty bundle -> 0, provider not consulted. */
   int n = kb_learning_synth_generate("q", NULL, "stub", 8, 2048, "user", "jbailes", "jbailes",
                                      NULL, 0);
   assert(n == 0);
   assert(model_calls == 0);

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
   assert(kb_store_synth_enqueue(id1) == 0);
   assert(kb_store_synth_enqueue(id2) == 0);
   assert(kb_store_synth_ops_count("pending") == 2);

   int n = kb_learning_synth_drain(16, NULL, "stub", 8, 2048);
   assert(n == 2); /* both ops processed */
   assert(kb_store_synth_ops_count("ok") == 2);
   assert(kb_store_synth_ops_count("pending") == 0);
   /* One candidate per processed op. */
   assert(kb_store_artifact_count("anti_pattern", "proposed") == 2);

   /* Drained queue is a no-op. */
   assert(kb_learning_synth_drain(16, NULL, "stub", 8, 2048) == 0);

   printf("  test_synth_drain: PASS\n");
}

static void test_opaque_command_is_retired(void)
{
   open_db();
   write_resp("{\"candidates\":[]}");
   seed("feedback_negative", "x", 1.0f);
   assert(kb_learning_synth_generate("q", STUB_CMD, "stub", 8, 2048, "user", "u", "u", NULL, 0) ==
          -1);
   assert(model_calls == 0);
}

static void test_incomplete_or_private_ir_never_writes(void)
{
   for (int mode = 1; mode <= 4; mode++)
   {
      open_db();
      write_resp("{\"candidates\":[{\"kind\":\"anti_pattern\",\"payload\":{\"title\":\"must not "
                 "write\"},\"confidence\":0.9}]}");
      seed("feedback_negative", "x", 1.0f);
      seed("guardrail_event", "y", 0.9f);
      model_mode = mode;
      assert(kb_learning_synth_generate("q", NULL, "stub", 8, 2048, "user", "u", "u", NULL, 0) ==
             -1);
      assert(model_calls == 1);
      assert(kb_store_artifact_count("anti_pattern", NULL) == 0);
      assert(kb_store_artifact_count("preference", NULL) == 0);
   }
}
static void test_domain_filters_and_strict_result(void)
{
   open_db();
   seed("feedback_negative", "x", 1.0f);
   write_resp("{\"candidates\":[{\"kind\":\"command\",\"payload\":{}},{\"kind\":\"preference\","
              "\"payload\":\"bad\"}]}");
   assert(kb_learning_synth_generate("q", NULL, "stub", 8, 2048, "user", "u", "u", NULL, 0) == 0);
   assert(kb_store_artifact_count("command", NULL) == 0);
   assert(kb_store_artifact_count("preference", NULL) == 0);
   write_resp("{\"candidates\":[],\"candidates\":[{\"kind\":\"preference\",\"payload\":{}}]}");
   assert(kb_learning_synth_generate("q", NULL, "stub", 8, 2048, "user", "u", "u", NULL, 0) == -1);
   assert(kb_store_artifact_count("preference", NULL) == 0);
}

int main(void)
{
   printf("test_learning_synth:\n");
   test_opaque_command_is_retired();
   test_incomplete_or_private_ir_never_writes();
   test_domain_filters_and_strict_result();
   test_synth_success();
   test_synth_failed_business_result();
   test_synth_empty_corpus();
   test_synth_drain();
   kb_store_test_shim_close();
   free(model_text);
   printf("test_learning_synth: ALL PASS\n");
   return 0;
}
