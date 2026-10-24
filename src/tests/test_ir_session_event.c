#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int captured;
static int capture(const aimee_session_event_t *event, void *context)
{
   assert(event->kind == AIMEE_SESSION_MESSAGE);
   assert(strcmp(event->message->role, "assistant") == 0);
   assert(event->message->n_blocks == 2);
   assert(event->message->blocks[0].type == AIMEE_BLK_THINKING);
   assert(event->message->blocks[1].type == AIMEE_BLK_TOOL_USE);
   captured++;
   return *(int *)context;
}

static int buffered_steps;
static int buffered_capture(const aimee_session_event_t *event, void *context)
{
   int fail_at = *(int *)context;
   buffered_steps++;
   if (buffered_steps == 2)
      assert(event->kind == AIMEE_SESSION_MESSAGE);
   else
   {
      assert(event->kind == AIMEE_SESSION_DELTA);
      assert(event->delta->type ==
             (buffered_steps == 1 ? AIMEE_DELTA_TURN_START : AIMEE_DELTA_TURN_STOP));
      if (buffered_steps == 3)
      {
         assert(event->delta->stop_reason == AIMEE_STOP_MAX_TOKENS);
         assert(event->delta->usage_cache_read == 9 && event->delta->usage_cache_write == 3);
      }
   }
   assert(!strcmp(event->message_id, "buffered"));
   return buffered_steps == fail_at ? -7 : 0;
}

static void test_owned_delta_assembly(void)
{
   aimee_ir_response_builder_t builder = {0};
   aimee_delta_t deltas[] = {{.type = AIMEE_DELTA_TURN_START},
                             {.type = AIMEE_DELTA_BLOCK_START,
                              .block_id = 0,
                              .kind = AIMEE_BLK_THINKING,
                              .text_delta = "reason",
                              .signature_delta = "sig-"},
                             {.type = AIMEE_DELTA_BLOCK_DELTA,
                              .block_id = 0,
                              .kind = AIMEE_BLK_THINKING,
                              .text_delta = " more",
                              .signature_delta = "complete"},
                             {.type = AIMEE_DELTA_BLOCK_STOP, .block_id = 0},
                             {.type = AIMEE_DELTA_BLOCK_START,
                              .block_id = 1,
                              .kind = AIMEE_BLK_TOOL_USE,
                              .tool_id = "opaque-id",
                              .tool_name = "Read"},
                             {.type = AIMEE_DELTA_BLOCK_DELTA,
                              .block_id = 1,
                              .kind = AIMEE_BLK_TOOL_USE,
                              .tool_args_delta = "{\"id\":9007199254740"},
                             {.type = AIMEE_DELTA_BLOCK_DELTA,
                              .block_id = 1,
                              .kind = AIMEE_BLK_TOOL_USE,
                              .tool_args_delta = "993}"},
                             {.type = AIMEE_DELTA_BLOCK_STOP, .block_id = 1},
                             {.type = AIMEE_DELTA_TURN_STOP,
                              .stop_reason = AIMEE_STOP_TOOL_USE,
                              .raw_stop_reason = "tool_use",
                              .usage_in = 12,
                              .usage_out = 4,
                              .usage_cache_read = 9}};
   for (size_t i = 0; i < sizeof deltas / sizeof deltas[0]; i++)
      assert(!aimee_ir_response_builder_apply(&builder, &deltas[i]));
   assert(builder.complete && builder.response.n_content == 2 &&
          builder.response.usage_cache_read == 9);
   assert(!strcmp(builder.response.content[0].text, "reason more") &&
          !strcmp(builder.response.content[0].thinking_signature, "sig-complete"));
   char *input = cJSON_PrintUnformatted(builder.response.content[1].tool_input);
   assert(input && !strcmp(input, "{\"id\":9007199254740993}"));
   free(input);
   assert(aimee_ir_response_builder_apply(&builder, &deltas[2]) < 0);
   aimee_ir_response_builder_free(&builder);
   assert(!aimee_ir_response_builder_apply(&builder, &deltas[0]));
   assert(aimee_ir_response_builder_apply(&builder, &deltas[0]) < 0);
   aimee_ir_response_builder_free(&builder);
   for (int invalid = 0; invalid < 3; invalid++)
   {
      assert(!aimee_ir_response_builder_apply(&builder, &deltas[0]));
      aimee_delta_t start = {.type = AIMEE_DELTA_BLOCK_START,
                             .block_id = 0,
                             .kind = AIMEE_BLK_TOOL_USE,
                             .tool_id = "call",
                             .tool_name = "Read"};
      assert(!aimee_ir_response_builder_apply(&builder, &start));
      if (invalid == 0)
         assert(aimee_ir_response_builder_apply(&builder, &deltas[8]) < 0);
      else if (invalid == 1)
         assert(aimee_ir_response_builder_apply(&builder, &start) < 0);
      else
      {
         aimee_delta_t bad = {.type = AIMEE_DELTA_BLOCK_DELTA,
                              .block_id = 0,
                              .kind = AIMEE_BLK_TOOL_USE,
                              .tool_args_delta = "{} trailing"};
         assert(!aimee_ir_response_builder_apply(&builder, &bad));
         bad.type = AIMEE_DELTA_BLOCK_STOP;
         assert(aimee_ir_response_builder_apply(&builder, &bad) < 0);
      }
      aimee_ir_response_builder_free(&builder);
   }
   puts("owned canonical delta assembly retains signatures/opaque arguments and refuses incomplete "
        "lifecycles");
}

static void test_argument_frame_admission(void)
{
   const char *arguments[] = {"{\"path\":\"before\\u0000after\"}", "{\"amount\":1,\"amount\":2}",
                              "{} {}", "{\"path\":\"\xff\"}"};
   for (unsigned i = 0; i < sizeof arguments / sizeof *arguments; i++)
   {
      aimee_ir_response_builder_t builder = {0};
      aimee_delta_t delta = {.type = AIMEE_DELTA_TURN_START};
      assert(!aimee_ir_response_builder_apply(&builder, &delta));
      delta = (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_START,
                              .kind = AIMEE_BLK_TOOL_USE,
                              .tool_id = "original",
                              .tool_name = "read_file"};
      assert(!aimee_ir_response_builder_apply(&builder, &delta));
      delta = (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_DELTA,
                              .kind = AIMEE_BLK_TOOL_USE,
                              .tool_args_delta = arguments[i]};
      assert(!aimee_ir_response_builder_apply(&builder, &delta));
      delta = (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_STOP};
      assert(aimee_ir_response_builder_apply(&builder, &delta) == -1);
      aimee_ir_response_builder_free(&builder);
   }
}

static void test_opaque_delta_metadata(void)
{
   aimee_ir_response_builder_t builder = {0};
   aimee_delta_t delta = {.type = AIMEE_DELTA_TURN_START};
   assert(!aimee_ir_response_builder_apply(&builder, &delta));
   char native_id[] = "rs_original", cipher[] = "opaque-cipher";
   delta = (aimee_delta_t){
       .type = AIMEE_DELTA_BLOCK_START, .kind = AIMEE_BLK_THINKING, .thinking_id = native_id};
   cJSON *json = aimee_ir_delta_json(&delta);
   assert(json && !strcmp(cJSON_GetObjectItem(json, "thinking_id")->valuestring, native_id));
   cJSON_Delete(json);
   assert(!aimee_ir_response_builder_apply(&builder, &delta));
   native_id[3] = 'X';
   delta.type = AIMEE_DELTA_BLOCK_DELTA;
   delta.thinking_id = "rs_original";
   delta.thinking_encrypted_content = cipher;
   assert(!aimee_ir_response_builder_apply(&builder, &delta));
   json = aimee_ir_delta_json(&delta);
   assert(json && !strcmp(cJSON_GetObjectItem(json, "encrypted_content")->valuestring, cipher));
   cJSON_Delete(json);
   size_t retained = builder.bytes;
   assert(!aimee_ir_response_builder_apply(&builder, &delta) && builder.bytes == retained);
   cipher[0] = 'X';
   assert(!strcmp(builder.response.content[0].thinking_id, "rs_original"));
   assert(!strcmp(builder.response.content[0].thinking_encrypted_content, "opaque-cipher"));
   assert(aimee_ir_response_builder_apply(&builder, &delta) < 0);
   delta.thinking_encrypted_content = "opaque-cipher";
   delta.thinking_id = "different-id";
   assert(aimee_ir_response_builder_apply(&builder, &delta) < 0);
   delta.thinking_id = "rs_original";
   delta.kind = AIMEE_BLK_TEXT;
   assert(!aimee_ir_delta_json(&delta));
   assert(aimee_ir_response_builder_apply(&builder, &delta) < 0);
   delta.kind = AIMEE_BLK_THINKING;
   delta.type = AIMEE_DELTA_BLOCK_STOP;
   assert(!aimee_ir_delta_json(&delta));
   assert(aimee_ir_response_builder_apply(&builder, &delta) < 0);
   delta.thinking_id = delta.thinking_encrypted_content = NULL;
   assert(!aimee_ir_response_builder_apply(&builder, &delta));
   delta = (aimee_delta_t){.type = AIMEE_DELTA_TURN_STOP, .stop_reason = AIMEE_STOP_END_TURN};
   assert(!aimee_ir_response_builder_apply(&builder, &delta));
   assert(builder.complete &&
          !strcmp(builder.response.content[0].thinking_encrypted_content, "opaque-cipher"));
   aimee_ir_response_builder_free(&builder);
}

static void test_response_snapshot(void)
{
   cJSON *arguments = cJSON_Parse("{\"marker\":0.123456789012345678901234567890}");
   cJSON *raw = cJSON_Parse("{\"opaque\":\"vendor data\"}");
   aimee_block_t blocks[] = {{.type = AIMEE_BLK_THINKING,
                              .text = "reason",
                              .thinking_signature = "signed",
                              .thinking_encrypted_content = "opaque"},
                             {.type = AIMEE_BLK_TOOL_USE,
                              .tool_id = "exact-id",
                              .tool_namespace = "aimee",
                              .tool_name = "read_file",
                              .tool_input = arguments}};
   aimee_response_t source = {.id = "source",
                              .model = "selected",
                              .role = "assistant",
                              .content = blocks,
                              .n_content = 2,
                              .stop_reason = AIMEE_STOP_TOOL_USE,
                              .raw_stop_reason = "tool_use",
                              .usage_in = 90,
                              .usage_out = 7,
                              .usage_cache_read = 80,
                              .usage_cache_write = 5,
                              .usage_reasoning = 3,
                              .raw = raw};
   aimee_response_t copy = {0};
   assert(!aimee_ir_response_clone(&source, &copy));
   assert(copy.content != blocks && copy.raw != raw && copy.content[1].tool_input != arguments);
   assert(copy.usage_cache_read == 80 && copy.usage_cache_write == 5 && copy.usage_reasoning == 3);
   assert(!strcmp(copy.content[0].thinking_signature, "signed") &&
          !strcmp(copy.content[0].thinking_encrypted_content, "opaque"));
   cJSON_ReplaceItemInObjectCaseSensitive(arguments, "marker", cJSON_CreateNumber(4));
   cJSON_Delete(arguments);
   cJSON_Delete(raw);
   char *exact = cJSON_PrintUnformatted(copy.content[1].tool_input);
   assert(exact && strstr(exact, "0.123456789012345678901234567890"));
   free(exact);
   assert(!aimee_ir_response_clone(&copy, &copy));
   aimee_response_t invalid = {.n_content = -1};
   assert(aimee_ir_response_clone(&invalid, &copy) < 0 && !strcmp(copy.id, "source"));
   aimee_response_free(&copy);
}
static cJSON *verification_fixture(void)
{
   cJSON *proof = cJSON_CreateObject();
   char digest[65];
   memset(digest, 'a', 64);
   digest[64] = 0;
   const char *fields[] = {"session_id", "turn_id", "actor", "scope_id"};
   for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i)
      cJSON_AddStringToObject(proof, fields[i], "fixture");
   cJSON_AddNumberToObject(proof, "version", 1);
   cJSON_AddStringToObject(proof, "scope_kind", "project");
   cJSON_AddStringToObject(proof, "isolation", "private_source_snapshot");
   cJSON_AddStringToObject(proof, "observed_at", "2026-10-20T12:00:00Z");
   cJSON_AddBoolToObject(proof, "completed", 1);
   const char *hashes[] = {"snapshot_commit", "final_commit",         "snapshot_tree",
                           "final_tree",      "configuration_sha256", "final_configuration_sha256",
                           "profile_sha256"};
   for (size_t i = 0; i < sizeof hashes / sizeof *hashes; ++i)
      cJSON_AddStringToObject(proof, hashes[i], digest);
   digest[32] = 0;
   cJSON_AddStringToObject(proof, "snapshot_id", digest);
   digest[32] = 'a';
   cJSON *step = cJSON_CreateObject();
   cJSON_AddStringToObject(step, "name", "unit");
   cJSON_AddStringToObject(step, "command_sha256", digest);
   cJSON_AddBoolToObject(step, "ran", 1);
   cJSON_AddBoolToObject(step, "skipped", 0);
   cJSON_AddStringToObject(step, "termination", "exited");
   cJSON_AddStringToObject(step, "exit_code", "0");
   cJSON_AddItemToArray(cJSON_AddArrayToObject(proof, "steps"), step);
   return proof;
}
static void test_verification_proofs(void)
{
   cJSON *proof = verification_fixture();
   assert(aimee_ir_verification_proof_valid(proof));
   cJSON *proofs = cJSON_CreateArray();
   cJSON_AddItemToArray(proofs, proof);
   aimee_session_event_t event = {.kind = AIMEE_SESSION_EXECUTION,
                                  .execution_status = "completed",
                                  .verification_proofs = proofs};
   cJSON *json = aimee_ir_session_event_json(&event);
   assert(json);
   assert(
       cJSON_Compare(proofs,
                     cJSON_GetObjectItemCaseSensitive(
                         cJSON_GetObjectItemCaseSensitive(json, "payload"), "verification_proofs"),
                     1));
   cJSON_Delete(json);
   const char *statuses[] = {"running", "failed", "cancelled"};
   for (size_t i = 0; i < sizeof statuses / sizeof *statuses; ++i)
   {
      event.execution_status = statuses[i];
      assert(!aimee_ir_session_event_json(&event));
   }
   event.execution_status = "completed";
   cJSON *step = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(proof, "steps"), 0);
   cJSON_ReplaceItemInObjectCaseSensitive(step, "exit_code", cJSON_CreateString("00"));
   assert(!aimee_ir_verification_proof_valid(proof));
   assert(!aimee_ir_session_event_json(&event));
   cJSON_ReplaceItemInObjectCaseSensitive(step, "exit_code", cJSON_CreateString("1"));
   assert(aimee_ir_verification_proof_valid(proof));
   cJSON_AddStringToObject(proof, "prompt", "not metadata");
   assert(!aimee_ir_verification_proof_valid(proof));
   cJSON_DeleteItemFromObjectCaseSensitive(proof, "prompt");
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(proof, "steps"), cJSON_Duplicate(step, 1));
   assert(!aimee_ir_verification_proof_valid(proof));
   cJSON_Delete(proofs);
   proofs = cJSON_CreateArray();
   event.verification_proofs = proofs;
   assert(!aimee_ir_session_event_json(&event));
   for (int i = 0; i < 9; ++i)
      cJSON_AddItemToArray(proofs, verification_fixture());
   assert(!aimee_ir_session_event_json(&event));
   cJSON_Delete(proofs);
}

static void test_refusal_terminal_json(void)
{
   aimee_refusal_details_t details = {.category = "future_category", .explanation = NULL};
   aimee_delta_t delta = {.type = AIMEE_DELTA_TURN_STOP,
                          .stop_reason = AIMEE_STOP_CONTENT_FILTER,
                          .raw_stop_reason = "refusal",
                          .refusal = &details};
   cJSON *json = aimee_ir_delta_json(&delta);
   assert(json);
   cJSON *refusal = cJSON_GetObjectItemCaseSensitive(json, "refusal");
   assert(cJSON_IsObject(refusal) && cJSON_GetArraySize(refusal) == 2);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refusal, "category")),
                  "future_category"));
   assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(refusal, "explanation")));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "raw_stop_reason")),
                  "refusal"));
   cJSON_Delete(json);
   details.category = NULL;
   json = aimee_ir_delta_json(&delta);
   assert(json && cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(
                      cJSON_GetObjectItemCaseSensitive(json, "refusal"), "category")));
   cJSON_Delete(json);
   delta.type = AIMEE_DELTA_TURN_START;
   assert(!aimee_ir_delta_json(&delta));
   delta.type = AIMEE_DELTA_TURN_STOP;
   delta.stop_reason = AIMEE_STOP_END_TURN;
   assert(!aimee_ir_delta_json(&delta));
   delta.refusal = NULL;
   json = aimee_ir_delta_json(&delta);
   assert(json && !cJSON_HasObjectItem(json, "refusal"));
   cJSON_Delete(json);
}

int main(void)
{
   test_refusal_terminal_json();
   test_verification_proofs();
   test_response_snapshot();
   test_owned_delta_assembly();
   test_opaque_delta_metadata();
   test_argument_frame_admission();
   aimee_block_t blocks[] = {
       {.type = AIMEE_BLK_THINKING,
        .text = "reason",
        .thinking_signature = "signature",
        .thinking_id = "rs_opaque",
        .thinking_encrypted_content = "ciphertext"},
       {.type = AIMEE_BLK_TOOL_USE,
        .tool_id = "opaque:42",
        .tool_name = "bash",
        .tool_namespace = "functions"},
       {.type = AIMEE_BLK_TOOL_RESULT, .tool_id = "opaque:42", .tool_is_error = 1},
       {.type = AIMEE_BLK_IMAGE,
        .media_type = "image/png",
        .media_ref = "data:image/png;base64,AA"},
       {.type = AIMEE_BLK_DOCUMENT, .media_type = "application/pdf", .media_ref = "artifact:pdf"},
   };
   blocks[1].tool_input = cJSON_Parse("{\"command\":\"pwd\"}");
   blocks[1].raw = cJSON_Parse("{\"native_secret\":\"not-session-content\"}");
   blocks[2].tool_result = cJSON_Parse("[{\"type\":\"text\",\"text\":\"failed\"}]");
   aimee_message_t message = {.role = "assistant", .blocks = blocks, .n_blocks = 5};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "msg-1", .message = &message};
   cJSON *json = aimee_ir_session_event_json(&event);
   assert(json);
   cJSON *payload = cJSON_GetObjectItemCaseSensitive(json, "payload");
   cJSON *content = cJSON_GetObjectItemCaseSensitive(payload, "blocks");
   assert(cJSON_GetArraySize(content) == 5);
   cJSON *use = cJSON_GetArrayItem(content, 1), *result = cJSON_GetArrayItem(content, 2);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(use, "tool_id")), "opaque:42") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(result, "tool_id")), "opaque:42") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "is_error")));
   assert(!cJSON_GetObjectItem(use, "raw"));
   assert(!cJSON_GetObjectItem(use, "native_secret"));
   blocks[0].cache_control = "ephemeral";
   blocks[0].context = (aimee_context_meta_t){.origin = AIMEE_CTX_ORIGIN_MODEL,
                                              .authority = AIMEE_CTX_AUTH_EVIDENCE,
                                              .trust = AIMEE_CTX_TRUST_UNVERIFIED,
                                              .sensitivity = AIMEE_CTX_SENS_CONFIDENTIAL,
                                              .model_visible = 1,
                                              .revision_domain = "session",
                                              .revision_scope = "scope",
                                              .revision_epoch = 18446744073709551615ULL};
   cJSON *encoded = aimee_ir_message_json(&message);
   assert(encoded);
   aimee_message_t decoded = {0};
   assert(aimee_ir_message_parse(encoded, &decoded) == 0);
   cJSON *again = aimee_ir_message_json(&decoded);
   assert(again && cJSON_Compare(encoded, again, 1));
   assert(decoded.blocks[0].context.revision_epoch == 18446744073709551615ULL);
   assert(decoded.blocks[0].cache_control && !strcmp(decoded.blocks[0].cache_control, "ephemeral"));
   assert(decoded.blocks[1].raw == NULL);
   aimee_message_free(&decoded);
   cJSON_Delete(again);
   cJSON *bad = cJSON_GetArrayItem(cJSON_GetObjectItem(encoded, "blocks"), 1);
   cJSON_ReplaceItemInObject(bad, "tool_id", cJSON_CreateNumber(42));
   assert(aimee_ir_message_parse(encoded, &decoded) == -1);
   assert(decoded.role == NULL && decoded.blocks == NULL && decoded.n_blocks == 0);
   cJSON_Delete(encoded);
   encoded = cJSON_Parse("{\"role\":\"assistant\",\"blocks\":[{\"type\":\"native_future_item\"}]}");
   assert(aimee_ir_message_parse(encoded, &decoded) == -1);
   cJSON_Delete(encoded);
   cJSON_Delete(json);

   aimee_delta_t delta = {.type = AIMEE_DELTA_BLOCK_DELTA,
                          .block_id = 7,
                          .kind = AIMEE_BLK_TOOL_USE,
                          .tool_args_delta = "{\"command\":"};
   event = (aimee_session_event_t){
       .kind = AIMEE_SESSION_DELTA, .message_id = "msg-2", .role = "assistant", .delta = &delta};
   json = aimee_ir_session_event_json(&event);
   assert(json);
   assert(!cJSON_GetObjectItem(json, "session_id"));
   payload = cJSON_GetObjectItem(json, "payload");
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload, "type")), "BLOCK_DELTA") == 0);
   assert(!cJSON_GetObjectItem(payload, "session_id"));
   cJSON_Delete(json);
   event.role = NULL;
   assert(!aimee_ir_session_event_json(&event));
   delta.type = (aimee_delta_type_t)99;
   assert(!aimee_ir_delta_json(&delta));
   delta = (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_START,
                           .kind = AIMEE_BLK_THINKING,
                           .block_id = 0,
                           .text_delta = "initial",
                           .signature_delta = "sig",
                           .redacted_content = "opaque"};
   json = aimee_ir_delta_json(&delta);
   assert(json &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "signature_delta")), "sig") &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "redacted_content")), "opaque"));
   cJSON_Delete(json);
   delta.kind = AIMEE_BLK_TEXT;
   assert(!aimee_ir_delta_json(&delta));
   blocks[1].tool_id = NULL;
   assert(!aimee_ir_block_json(&blocks[1]));
   blocks[1].tool_id = "opaque:42";
   aimee_block_t unknown = {.type = AIMEE_BLK_UNKNOWN};
   assert(!aimee_ir_block_json(&unknown));

   cJSON *control = cJSON_Parse("{\"options\":[\"allow\",\"deny\"]}");
   event = (aimee_session_event_t){.kind = AIMEE_SESSION_CONTROL_REQUEST,
                                   .request_id = "permission-9",
                                   .control_kind = "permission",
                                   .control = control};
   json = aimee_ir_session_event_json(&event);
   assert(json);
   assert(!cJSON_GetObjectItem(json, "role"));
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "kind")), "control_request") == 0);
   cJSON_Delete(json);
   event.request_id = NULL;
   assert(!aimee_ir_session_event_json(&event));
   cJSON_Delete(control);

   event = (aimee_session_event_t){.kind = AIMEE_SESSION_EXECUTION, .execution_status = "running"};
   json = aimee_ir_session_event_json(&event);
   assert(json);
   assert(!cJSON_GetObjectItem(json, "message_id"));
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "kind")), "execution") == 0);
   cJSON_Delete(json);
   event.execution_status = "invented";
   assert(!aimee_ir_session_event_json(&event));
   delta =
       (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_START, .block_id = 0, .kind = AIMEE_BLK_TOOL_USE};
   assert(!aimee_ir_delta_json(&delta));
   delta.kind = AIMEE_BLK_UNKNOWN;
   assert(!aimee_ir_delta_json(&delta));
   delta.kind = AIMEE_BLK_TEXT;
   delta.block_id = 4096;
   assert(!aimee_ir_delta_json(&delta));
   delta = (aimee_delta_t){.type = AIMEE_DELTA_BLOCK_DELTA,
                           .block_id = 0,
                           .kind = AIMEE_BLK_TEXT,
                           .tool_args_delta = "{"};
   assert(!aimee_ir_delta_json(&delta));

   char digest[65], binding[199];
   memset(digest, 'a', 64);
   digest[64] = 0;
   snprintf(binding, sizeof binding, "ap1:%s:%s:%s", digest, digest, digest);
   event = (aimee_session_event_t){.kind = AIMEE_SESSION_REPLAY_PROOF,
                                   .replay_scope = digest,
                                   .replay_digest = digest,
                                   .replay_wire = "anthropic",
                                   .replay_binding = binding};
   json = aimee_ir_session_event_json(&event);
   assert(json && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "kind")), "replay_proof"));
   assert(!cJSON_GetObjectItem(json, "message_id") && !cJSON_GetObjectItem(json, "role"));
   cJSON_Delete(json);
   event.replay_binding = "invented";
   assert(!aimee_ir_session_event_json(&event));
   event.replay_wire = "responses";
   assert(!aimee_ir_session_event_json(&event));
   event.replay_binding = NULL;
   json = aimee_ir_session_event_json(&event);
   assert(json);
   cJSON_Delete(json);
   event.replay_scope = "not-a-digest";
   assert(!aimee_ir_session_event_json(&event));

   event = (aimee_session_event_t){.kind = AIMEE_SESSION_PROJECTION,
                                   .projection_model = "model-b",
                                   .projection_boundary = "request",
                                   .projection_reasoning = 2,
                                   .projection_opaque = 1};
   json = aimee_ir_session_event_json(&event);
   assert(json && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "kind")), "projection"));
   assert(!cJSON_GetObjectItem(json, "message_id") && !cJSON_GetObjectItem(json, "role"));
   cJSON *report = cJSON_GetObjectItem(json, "payload");
   assert(cJSON_GetObjectItem(report, "omitted_reasoning_blocks")->valueint == 2);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(report, "boundary")), "request"));
   cJSON_Delete(json);
   event.projection_opaque = 3;
   assert(!aimee_ir_session_event_json(&event));
   event.projection_opaque = -1;
   assert(!aimee_ir_session_event_json(&event));

   event.projection_reasoning = 0;
   event.projection_opaque = 0;
   event.projection_cache_controls = 3;
   json = aimee_ir_session_event_json(&event);
   assert(json &&
          cJSON_GetObjectItem(cJSON_GetObjectItem(json, "payload"), "omitted_cache_controls")
                  ->valueint == 3);
   cJSON_Delete(json);
   event.projection_cache_controls = -1;
   assert(!aimee_ir_session_event_json(&event));
   event.projection_cache_controls = 0;
   assert(!aimee_ir_session_event_json(&event));

   event = (aimee_session_event_t){.kind = AIMEE_SESSION_CHILD_EXECUTION,
                                   .child_session = "private-child",
                                   .child_turn = "child-run",
                                   .child_role = "review",
                                   .execution_status = "running"};
   json = aimee_ir_session_event_json(&event);
   assert(json &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "kind")), "child_execution"));
   const cJSON *child = cJSON_GetObjectItem(json, "payload");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(child, "session_id")), "private-child"));
   assert(!cJSON_GetObjectItem(json, "message_id") && !cJSON_GetObjectItem(child, "prompt"));
   cJSON_Delete(json);
   event.child_role = NULL;
   assert(!aimee_ir_session_event_json(&event));
   event.child_role = "review";
   event.execution_status = "invented";
   assert(!aimee_ir_session_event_json(&event));

   aimee_tool_t child_tool = {
       .name = "Read", .strict = AIMEE_STRICT_NULL, .schema = cJSON_Parse("{\"type\":\"object\"}")};
   aimee_tool_t request_tools[2] = {{.kind = AIMEE_TOOL_NAMESPACE,
                                     .name = "functions",
                                     .children = &child_tool,
                                     .n_children = 1},
                                    {.kind = AIMEE_TOOL_HOSTED,
                                     .provider_wire = AIMEE_WIRE_RESPONSES,
                                     .native_type = "web_search",
                                     .native_options = cJSON_Parse("{\"hosted_setting\":true}")}};
   aimee_block_t request_text = {.type = AIMEE_BLK_TEXT, .text = "question"};
   aimee_message_t request_message = {.role = "user", .blocks = &request_text, .n_blocks = 1};
   aimee_request_t request = {
       .model = "model",
       .frontend = AIMEE_WIRE_RESPONSES,
       .messages = &request_message,
       .n_messages = 1,
       .tools = request_tools,
       .n_tools = 2,
       .has_max_tokens = 1,
       .max_tokens = 77,
       .reasoning_effort = AIMEE_EFFORT_HIGH,
       .tool_choice = AIMEE_TOOL_CHOICE_TOOL,
       .tool_choice_name = "Read",
       .raw = cJSON_Parse("{\"model_auth_secret_must_not_enter_canonical_snapshot\":true}")};
   json = aimee_ir_request_json(&request);
   char *snapshot = json ? cJSON_PrintUnformatted(json) : NULL;
   assert(snapshot && strstr(snapshot, "hosted_setting") && !strstr(snapshot, "model_auth_secret"));
   const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(json, "tools");
   const cJSON *child_json = cJSON_GetArrayItem(
       cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(catalog, 0), "children"), 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(child_json, "strict")),
                  "null"));
   free(snapshot);
   cJSON_Delete(json);
   json = aimee_ir_policy_view_json(&request);
   snapshot = json ? cJSON_PrintUnformatted(json) : NULL;
   assert(snapshot && strstr(snapshot, "web_search") && !strstr(snapshot, "hosted_setting") &&
          !strstr(snapshot, "schema"));
   free(snapshot);
   cJSON_Delete(json);
   cJSON_Delete(request.raw);
   cJSON_Delete(child_tool.schema);
   cJSON_Delete(request_tools[1].native_options);

   int failure = -7;
   aimee_response_t response = {.role = "assistant", .content = blocks, .n_content = 2};
   aimee_ir_session_set_sink(capture, &failure);
   assert(aimee_ir_session_emit_response(&response, "msg-3") == -7);
   assert(captured == 1);
   assert(aimee_ir_session_capture_failed());
   aimee_ir_session_begin_task();
   assert(aimee_ir_session_capture_failed()); /* Nested work cannot clear a live owner. */
   aimee_ir_session_restore_sink(NULL, NULL, 1);
   assert(aimee_ir_session_capture_failed());
   aimee_ir_session_begin_task();
   assert(!aimee_ir_session_capture_failed());
   aimee_ir_session_set_sink(capture, &failure);
   void *context = NULL;
   aimee_ir_session_sink_fn observed_sink = aimee_ir_session_get_sink(&context);
   int matches_capture = observed_sink == capture;
   assert(matches_capture && context == &failure);
   aimee_ir_session_set_sink(NULL, NULL);
   observed_sink = aimee_ir_session_get_sink(&context);
   assert(observed_sink == NULL && context == NULL);
   assert(!aimee_ir_session_capture_failed());
   assert(aimee_ir_session_emit_response(&response, "msg-3") == 0);
   cJSON_Delete(blocks[1].tool_input);
   cJSON_Delete(blocks[1].raw);
   cJSON_Delete(blocks[2].tool_result);
   /* Reused IDs belong to their current ordered tool cycle. A prior result
    * must not hide a missing result in a later turn. Repair is idempotent. */
   cJSON *history = cJSON_Parse(
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"thinking\",\"text\":\"reason\","
       "\"signature\":\"sig\"},"
       "{\"type\":\"tool_use\",\"tool_id\":\"same\",\"name\":\"read_file\",\"input\":{}}]},"
       "{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"tool_id\":\"same\",\"content\":"
       "\"ok\",\"is_error\":false}]},"
       "{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":\"same\",\"name\":"
       "\"read_file\",\"input\":{}}]},"
       "{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"next\"}]},"
       "{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"tool_id\":\"orphan\","
       "\"content\":\"bad\",\"is_error\":false}]}]");
   assert(history);
   char *unresolved_before = cJSON_PrintUnformatted(history);
   assert(unresolved_before && aimee_ir_history_admit(history) == -1);
   char *unresolved_after = cJSON_PrintUnformatted(history);
   assert(unresolved_after && !strcmp(unresolved_before, unresolved_after));
   free(unresolved_before);
   free(unresolved_after);
   assert(aimee_ir_history_repair(history) == 2);
   assert(cJSON_GetArraySize(history) == 5);
   cJSON *repaired =
       cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(history, 3), "blocks"), 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(repaired, "tool_id")), "same"));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(repaired, "is_error")));
   const char *diagnostic = cJSON_GetStringValue(cJSON_GetObjectItem(repaired, "content"));
   assert(diagnostic && strstr(diagnostic, "outcome is unknown") &&
          !strstr(diagnostic, "cancelled"));
   const cJSON *diagnostic_context = cJSON_GetObjectItem(repaired, "context");
   assert(cJSON_GetObjectItem(diagnostic_context, "origin")->valueint == AIMEE_CTX_ORIGIN_PLATFORM);
   assert(cJSON_GetObjectItem(diagnostic_context, "authority")->valueint ==
          AIMEE_CTX_AUTH_DIAGNOSTIC);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(diagnostic_context, "revision_scope")),
                  "unresolved-tool-result"));
   assert(!strcmp(
       cJSON_GetStringValue(cJSON_GetObjectItem(
           cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(history, 1), "blocks"), 0),
           "content")),
       "ok"));
   assert(aimee_ir_history_repair(history) == 0);
   assert(aimee_ir_history_admit(history) == -1); /* Diagnostics do not become receipts. */
   cJSON *resolved = cJSON_Parse(
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":\"same\",\"name\":"
       "\"write_file\",\"input\":{}}]},{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\","
       "\"tool_id\":\"same\",\"content\":\"actual receipt\",\"is_error\":false}]}]");
   assert(resolved && !aimee_ir_history_admit(resolved));
   cJSON_Delete(resolved);
   assert(!strcmp(
       cJSON_GetStringValue(cJSON_GetObjectItem(
           cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(history, 0), "blocks"), 0),
           "signature")),
       "sig"));
   cJSON_Delete(history);
   for (int fail_at = 0; fail_at <= 3; fail_at++)
   {
      aimee_response_t buffered = {.role = "assistant",
                                   .content = blocks,
                                   .n_content = 1,
                                   .stop_reason = AIMEE_STOP_MAX_TOKENS,
                                   .usage_cache_read = 9,
                                   .usage_cache_write = 3};
      buffered_steps = 0;
      aimee_ir_session_set_sink(buffered_capture, &fail_at);
      assert(aimee_ir_session_emit_buffered_response(&buffered, "buffered") == (fail_at ? -7 : 0));
      assert(buffered_steps == (fail_at ? fail_at : 3));
   }
   aimee_ir_session_set_sink(NULL, NULL);
   aimee_ir_response_builder_t grouped_builder = {0};
   aimee_delta_t grouped = {.type = AIMEE_DELTA_TURN_START};
   assert(!aimee_ir_response_builder_apply(&grouped_builder, &grouped));
   grouped = (aimee_delta_t){
       .type = AIMEE_DELTA_BLOCK_START, .kind = AIMEE_BLK_TEXT, .block_id = 0, .message_group = 2};
   cJSON *grouped_json = aimee_ir_delta_json(&grouped);
   assert(grouped_json && cJSON_GetObjectItem(grouped_json, "message_group")->valueint == 2);
   cJSON_Delete(grouped_json);
   assert(!aimee_ir_response_builder_apply(&grouped_builder, &grouped));
   assert(grouped_builder.response.content[0].message_group == 2);
   grouped.type = AIMEE_DELTA_BLOCK_DELTA;
   assert(!aimee_ir_delta_json(&grouped));
   assert(aimee_ir_response_builder_apply(&grouped_builder, &grouped) < 0);
   grouped.type = AIMEE_DELTA_BLOCK_START;
   grouped.kind = AIMEE_BLK_THINKING;
   assert(!aimee_ir_delta_json(&grouped));
   grouped.kind = AIMEE_BLK_TEXT;
   grouped.message_group = -1;
   assert(!aimee_ir_delta_json(&grouped));
   aimee_ir_response_builder_free(&grouped_builder);
   char checkpoint_digest[65], checkpoint_claim[33];
   memset(checkpoint_digest, 'a', 64);
   checkpoint_digest[64] = 0;
   memset(checkpoint_claim, 'b', 32);
   checkpoint_claim[32] = 0;
   aimee_native_checkpoint_t checkpoint = {.scope = checkpoint_digest,
                                           .context_digest = checkpoint_digest,
                                           .prefix_digest = checkpoint_digest,
                                           .claim = checkpoint_claim,
                                           .harness = "codex",
                                           .native_id = "opaque:native",
                                           .message_count = 2};
   aimee_session_event_t checkpoint_event = {.kind = AIMEE_SESSION_NATIVE_CHECKPOINT,
                                             .native_checkpoint = &checkpoint};
   cJSON *checkpoint_json = aimee_ir_session_event_json(&checkpoint_event);
   assert(checkpoint_json &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(checkpoint_json, "kind")),
                  "native_checkpoint"));
   cJSON_Delete(checkpoint_json);
   checkpoint.message_count = 0;
   assert(!aimee_ir_session_event_json(&checkpoint_event));
   checkpoint.message_count = 2;
   checkpoint.harness = "unknown";
   assert(!aimee_ir_session_event_json(&checkpoint_event));
   checkpoint.harness = "claude";
   checkpoint.native_id = "";
   assert(!aimee_ir_session_event_json(&checkpoint_event));
   aimee_session_event_t compaction_event = {.kind = AIMEE_SESSION_NATIVE_COMPACTION,
                                             .native_harness = "claude"};
   cJSON *compaction_json = aimee_ir_session_event_json(&compaction_event);
   assert(compaction_json);
   cJSON_Delete(compaction_json);
   compaction_event.native_harness = "invented";
   compaction_json = aimee_ir_session_event_json(&compaction_event);
   assert(!compaction_json);
   compaction_event.native_harness = NULL;
   compaction_json = aimee_ir_session_event_json(&compaction_event);
   assert(!compaction_json);

   aimee_delta_t unknown_usage = {
       .type = AIMEE_DELTA_TURN_STOP, .stop_reason = AIMEE_STOP_END_TURN, .usage_unobserved = 1};
   cJSON *unknown_json = aimee_ir_delta_json(&unknown_usage);
   assert(unknown_json && !cJSON_GetObjectItem(unknown_json, "usage_in") &&
          !cJSON_GetObjectItem(unknown_json, "usage_out"));
   cJSON_Delete(unknown_json);
   aimee_ir_response_builder_t unknown_builder = {0};
   aimee_delta_t unknown_start = {.type = AIMEE_DELTA_TURN_START};
   assert(!aimee_ir_response_builder_apply(&unknown_builder, &unknown_start));
   assert(!aimee_ir_response_builder_apply(&unknown_builder, &unknown_usage));
   assert(unknown_builder.response.usage_unobserved);
   aimee_ir_response_builder_free(&unknown_builder);
   unknown_usage.usage_in = 1;
   assert(!aimee_ir_delta_json(&unknown_usage));
   unknown_usage.usage_in = 0;
   unknown_usage.usage_unobserved = 0;
   unknown_json = aimee_ir_delta_json(&unknown_usage);
   assert(unknown_json && cJSON_GetObjectItem(unknown_json, "usage_in") &&
          cJSON_GetObjectItem(unknown_json, "usage_out"));
   cJSON_Delete(unknown_json);

   puts("IR session event tests passed");
   return 0;
}
