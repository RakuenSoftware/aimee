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

int main(void)
{
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
   assert(aimee_ir_session_get_sink(&context) == capture && context == &failure);
   aimee_ir_session_set_sink(NULL, NULL);
   assert(aimee_ir_session_get_sink(&context) == NULL && context == NULL);
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
   assert(aimee_ir_history_repair(history) == 2);
   assert(cJSON_GetArraySize(history) == 5);
   cJSON *repaired =
       cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(history, 3), "blocks"), 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(repaired, "tool_id")), "same"));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(repaired, "is_error")));
   assert(aimee_ir_history_repair(history) == 0);
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
   puts("IR session event tests passed");
   return 0;
}
