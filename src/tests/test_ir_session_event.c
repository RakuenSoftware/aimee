#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
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

int main(void)
{
   aimee_block_t blocks[] = {
       {.type = AIMEE_BLK_THINKING, .text = "reason", .thinking_signature = "signature"},
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

   int failure = -7;
   aimee_response_t response = {.role = "assistant", .content = blocks, .n_content = 2};
   aimee_ir_session_set_sink(capture, &failure);
   assert(aimee_ir_session_emit_response(&response, "msg-3") == -7);
   assert(captured == 1);
   void *context = NULL;
   assert(aimee_ir_session_get_sink(&context) == capture && context == &failure);
   aimee_ir_session_set_sink(NULL, NULL);
   assert(aimee_ir_session_get_sink(&context) == NULL && context == NULL);
   assert(aimee_ir_session_emit_response(&response, "msg-3") == 0);
   cJSON_Delete(blocks[1].tool_input);
   cJSON_Delete(blocks[1].raw);
   cJSON_Delete(blocks[2].tool_result);
   puts("IR session event tests passed");
   return 0;
}
