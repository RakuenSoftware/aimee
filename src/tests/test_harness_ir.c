#include <aimee/translation/harness_ir.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *events;
static int reject;
static int capture(const aimee_session_event_t *event, void *unused)
{
   (void)unused;
   if (reject)
      return -1;
   cJSON *json = aimee_ir_session_event_json(event);
   assert(json);
   cJSON_AddItemToArray(events, json);
   return 0;
}
/* Each fixture is an independent execution; a failing fixture cannot poison
 * another, while a nested owner within the same execution must keep the latch. */
static void next_execution(void)
{
   aimee_ir_session_set_sink(NULL, NULL);
   aimee_ir_session_set_sink(capture, NULL);
}
static int frame(const char *json, aimee_harness_ir_t *s, int codex)
{
   cJSON *f = cJSON_Parse(json);
   assert(f);
   int rc = codex == 2 ? aimee_acp_frame_ir(f, s)
            : codex    ? aimee_codex_frame_ir(f, s)
                       : aimee_claude_frame_ir(f, s);
   cJSON_Delete(f);
   return rc;
}
static const char *string(const cJSON *o, const char *name)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, name));
}
static cJSON *block(int index)
{
   cJSON *e = cJSON_GetArrayItem(events, index);
   return cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(e, "payload"), "blocks"), 0);
}
static void claude(void)
{
   aimee_harness_ir_t s = {0};
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\",\"message\":{"
                "\"id\":\"a1\",\"role\":\"assistant\"}}}",
                &s, 0) == 0);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\",\"index\":"
                "0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}}",
                &s, 0) == 0);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"index\":"
                "0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Inspecting\"}}}",
                &s, 0) == 0);
   assert(
       frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_stop\",\"index\":0}}",
             &s, 0) == 0);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"message_delta\",\"delta\":{"
                "\"stop_reason\":\"end_turn\"}}}",
                &s, 0) == 0);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"message_stop\"}}", &s, 0) == 0);
   assert(cJSON_GetArraySize(events) == 5);
   assert(!strcmp(string(cJSON_GetArrayItem(events, 0), "message_id"), "a1"));
   assert(frame("{\"type\":\"assistant\",\"message\":{\"id\":\"a1\",\"role\":\"assistant\","
                "\"content\":[{\"type\":\"thinking\",\"thinking\":\"Reason\",\"signature\":"
                "\"signed\"},{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Bash\",\"input\":{"
                "\"command\":\"pwd\"}}],\"stop_reason\":\"tool_use\"}}",
                &s, 0) == 0);
   assert(!strcmp(string(block(5), "type"), "thinking"));
   assert(!strcmp(string(block(5), "signature"), "signed"));
   assert(frame("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_"
                "result\",\"tool_use_id\":\"t1\",\"content\":\"/workspace\",\"is_error\":true}]}}",
                &s, 0) == 0);
   assert(!strcmp(string(block(6), "tool_id"), "t1"));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(block(6), "is_error")));
   assert(frame("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"prompt echo\"}}",
                &s, 0) == 0);
   assert(cJSON_GetArraySize(events) == 7);
   reject = 1;
   assert(frame("{\"type\":\"assistant\",\"message\":{\"id\":\"a2\",\"role\":\"assistant\","
                "\"content\":[{\"type\":\"text\",\"text\":\"Fail closed\"}]}}",
                &s, 0) < 0);
   reject = 0;
   aimee_harness_ir_free(&s);
}
static void claude_block_snapshots(void)
{
   aimee_harness_ir_t s = {0};
   int before = cJSON_GetArraySize(events);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\",\"message\":{"
                "\"id\":\"multi\",\"role\":\"assistant\"}}}",
                &s, 0) == 0);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\",\"index\":"
                "0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}}",
                &s, 0) == 0);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"index\":"
                "0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hello\"}}}",
                &s, 0) == 0);
   assert(frame("{\"type\":\"assistant\",\"message\":{\"id\":\"multi\",\"role\":\"assistant\","
                "\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}}",
                &s, 0) == 0);
   assert(cJSON_GetArraySize(events) == before + 3); /* snapshot cannot close the message yet */
   assert(
       frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_stop\",\"index\":0}}",
             &s, 0) == 0);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\",\"index\":"
                "1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"Read\","
                "\"input\":{}}}}",
                &s, 0) == 0);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"index\":"
                "1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{}\"}}}",
                &s, 0) == 0);
   assert(
       frame("{\"type\":\"assistant\",\"message\":{\"id\":\"multi\",\"role\":\"assistant\","
             "\"content\":[{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"Read\",\"input\":{}}]}}",
             &s, 0) == 0);
   assert(
       frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_stop\",\"index\":1}}",
             &s, 0) == 0);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"message_delta\",\"delta\":{"
                "\"stop_reason\":\"end_turn\"}}}",
                &s, 0) == 0);
   assert(frame("{\"type\":\"stream_event\",\"event\":{\"type\":\"message_stop\"}}", &s, 0) == 0);
   assert(cJSON_GetArraySize(events) == before + 9);
   cJSON *last = cJSON_GetArrayItem(events, before + 8);
   cJSON *blocks = cJSON_GetObjectItem(cJSON_GetObjectItem(last, "payload"), "blocks");
   assert(cJSON_GetArraySize(blocks) == 2);
   assert(!strcmp(string(cJSON_GetArrayItem(blocks, 0), "text"), "hello"));
   assert(!strcmp(string(cJSON_GetArrayItem(blocks, 1), "tool_id"), "t2"));
   aimee_harness_ir_free(&s);
}
static void codex(void)
{
   aimee_harness_ir_t s = {0};
   int before = cJSON_GetArraySize(events);
   assert(frame("{\"method\":\"item/"
                "started\",\"params\":{\"item\":{\"id\":\"cmd:1\",\"type\":\"commandExecution\","
                "\"command\":\"pwd\",\"cwd\":\"/work\"}}}",
                &s, 1) == 0);
   assert(frame("{\"method\":\"item/"
                "completed\",\"params\":{\"item\":{\"id\":\"cmd:1\",\"type\":\"commandExecution\","
                "\"aggregatedOutput\":\"denied\",\"exitCode\":1,\"status\":\"failed\"}}}",
                &s, 1) == 0);
   assert(!strcmp(string(block(before), "tool_id"), string(block(before + 1), "tool_id")));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(block(before + 1), "is_error")));
   assert(frame("{\"method\":\"item/agentMessage/"
                "delta\",\"params\":{\"itemId\":\"answer\",\"delta\":\"First\"}}",
                &s, 1) == 0);
   assert(frame("{\"method\":\"item/"
                "completed\",\"params\":{\"item\":{\"id\":\"answer\",\"type\":\"agentMessage\","
                "\"text\":\"First answer\"}}}",
                &s, 1) == 0);
   assert(frame("{\"id\":7,\"method\":\"item/commandExecution/"
                "requestApproval\",\"params\":{\"command\":\"pwd\"}}",
                &s, 1) == 0);
   int n = cJSON_GetArraySize(events);
   assert(frame("{\"id\":7,\"result\":{\"server_response\":true}}", &s, 1) == 0);
   assert(cJSON_GetArraySize(events) == n); /* inbound id namespace is distinct */
   cJSON *reply = cJSON_Parse("{\"id\":7,\"result\":{\"decision\":\"accept\"}}");
   assert(aimee_codex_reply_ir(reply, &s) == 0);
   cJSON_Delete(reply);
   assert(cJSON_GetArraySize(events) == n + 1);
   assert(!strcmp(string(cJSON_GetArrayItem(events, n), "kind"), "control_response"));
   assert(frame("{\"method\":\"item/"
                "completed\",\"params\":{\"item\":{\"id\":\"unknown\",\"type\":\"futureItem\"}}}",
                &s, 1) < 0);
   aimee_harness_ir_free(&s);
}
static void codex_tool_results(void)
{
   const struct
   {
      const char *item;
      int error;
      const char *output;
   } cases[] = {
       {"{\"type\":\"commandExecution\",\"exitCode\":7}", 1, NULL},
       {"{\"type\":\"commandExecution\",\"exitCode\":0}", 0, NULL},
       {"{\"type\":\"commandExecution\",\"exitCode\":null}", 0, NULL},
       {"{\"type\":\"dynamicToolCall\",\"tool\":\"lookup\",\"namespace\":\"catalog\","
        "\"success\":false,\"contentItems\":[{\"type\":\"inputText\",\"text\":\"no match\"}]}",
        1, "no match"},
       {"{\"type\":\"dynamicToolCall\",\"tool\":\"lookup\",\"namespace\":\"catalog\","
        "\"success\":true,\"contentItems\":[{\"type\":\"inputText\",\"text\":\"found\"}]}",
        0, "found"},
       {"{\"type\":\"mcpToolCall\",\"server\":\"fixture\",\"tool\":\"lookup\","
        "\"result\":{\"isError\":true,\"content\":[]}}",
        1, NULL},
       {"{\"type\":\"mcpToolCall\",\"server\":\"fixture\",\"tool\":\"lookup\","
        "\"error\":{\"message\":\"transport failed\"}}",
        1, NULL},
       {"{\"type\":\"mcpToolCall\",\"server\":\"fixture\",\"tool\":\"lookup\","
        "\"result\":{\"content\":[]},\"error\":null}",
        0, NULL}};
   for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
   {
      aimee_harness_ir_t state = {0};
      cJSON *item = cJSON_Parse(cases[i].item);
      assert(item);
      cJSON_AddStringToObject(item, "id", "full-call:identity");
      cJSON_AddStringToObject(item, "status", "completed");
      cJSON *arguments = cJSON_AddObjectToObject(item, "arguments");
      cJSON_AddNumberToObject(arguments, "value", 7);
      cJSON *event = cJSON_CreateObject();
      cJSON_AddStringToObject(event, "method", "item/started");
      cJSON *params = cJSON_AddObjectToObject(event, "params");
      cJSON_AddItemToObject(params, "item", item);
      int before = cJSON_GetArraySize(events);
      assert(aimee_codex_frame_ir(event, &state) == 0);
      cJSON *use = block(before);
      if (strcmp(string(item, "type"), "commandExecution"))
      {
         assert(cJSON_Compare(cJSON_GetObjectItem(use, "input"), arguments, 1));
         assert(!strcmp(string(use, "namespace"),
                        !strcmp(string(item, "type"), "mcpToolCall") ? "mcp__fixture" : "catalog"));
      }
      cJSON_SetValuestring(cJSON_GetObjectItem(event, "method"), "item/completed");
      assert(aimee_codex_frame_ir(event, &state) == 0);
      cJSON *result = block(before + 1);
      assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "is_error")) == cases[i].error);
      assert(!strcmp(string(use, "tool_id"), string(result, "tool_id")));
      if (cases[i].output)
      {
         const cJSON *content = cJSON_GetObjectItem(result, "content");
         const cJSON *output = cJSON_GetArrayItem(cJSON_GetObjectItem(content, "output"), 0);
         assert(!strcmp(string(output, "text"), cases[i].output));
      }
      cJSON_Delete(event);
      aimee_harness_ir_free(&state);
   }
}
static void acp(void)
{
   aimee_harness_ir_t s = {0};
   int before = cJSON_GetArraySize(events);
   assert(frame("{\"method\":\"session/"
                "update\",\"params\":{\"update\":{\"sessionUpdate\":\"agent_thought_chunk\","
                "\"content\":{\"type\":\"text\",\"text\":\"Inspect\"}}}}",
                &s, 2) == 0);
   assert(frame("{\"method\":\"session/"
                "update\",\"params\":{\"update\":{\"sessionUpdate\":\"agent_message_chunk\","
                "\"content\":{\"type\":\"text\",\"text\":\"Answer\"}}}}",
                &s, 2) == 0);
   assert(frame("{\"method\":\"session/"
                "update\",\"params\":{\"update\":{\"sessionUpdate\":\"tool_call\",\"toolCallId\":"
                "\"acp:1\",\"title\":\"Read\",\"status\":\"in_progress\",\"rawInput\":{\"path\":"
                "\"README\"}}}}",
                &s, 2) == 0);
   int use = cJSON_GetArraySize(events) - 1;
   assert(!strcmp(string(block(use), "type"), "tool_use"));
   assert(
       frame(
           "{\"method\":\"session/"
           "update\",\"params\":{\"update\":{\"sessionUpdate\":\"tool_call_update\",\"toolCallId\":"
           "\"acp:1\",\"status\":\"completed\",\"rawOutput\":{\"text\":\"file contents\"}}}}",
           &s, 2) == 0);
   int result = cJSON_GetArraySize(events) - 1;
   assert(!strcmp(string(block(result), "tool_id"), "acp:1"));
   assert(!strcmp(string(block(result), "type"), "tool_result"));
   assert(!strcmp(string(block(result - 1), "name"), "Read"));
   assert(frame("{\"id\":9,\"method\":\"session/"
                "request_permission\",\"params\":{\"options\":[{\"optionId\":\"allow\"}]}}",
                &s, 2) == 0);
   cJSON *reply = cJSON_Parse(
       "{\"id\":9,\"result\":{\"outcome\":{\"outcome\":\"selected\",\"optionId\":\"allow\"}}}");
   assert(aimee_codex_reply_ir(reply, &s) == 0);
   cJSON_Delete(reply);
   assert(!strcmp(string(cJSON_GetArrayItem(events, cJSON_GetArraySize(events) - 1), "kind"),
                  "control_response"));
   assert(frame("{\"method\":\"session/"
                "update\",\"params\":{\"update\":{\"sessionUpdate\":\"user_message_chunk\","
                "\"content\":{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"AA==\"}}}}",
                &s, 2) == 0);
   assert(!strcmp(string(block(cJSON_GetArraySize(events) - 1), "media_ref"),
                  "data:image/png;base64,AA=="));
   assert(frame("{\"method\":\"session/"
                "update\",\"params\":{\"update\":{\"sessionUpdate\":\"unknown_content\"}}}",
                &s, 2) < 0);
   assert(cJSON_GetArraySize(events) > before);
   aimee_harness_ir_free(&s);
}
static void control_outputs(void)
{
   cJSON *request = cJSON_Parse("{\"tool_name\":\"Read\",\"input\":{\"path\":\"source.c\"}}");
   cJSON *decision = cJSON_Parse("{\"decision\":\"allow\"}");
   cJSON *frame = aimee_claude_control_reply("native-permission", request, decision);
   assert(frame);
   cJSON *response = cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "response"), "response");
   assert(!strcmp(string(response, "behavior"), "allow"));
   assert(cJSON_Compare(cJSON_GetObjectItem(response, "updatedInput"),
                        cJSON_GetObjectItem(request, "input"), 1));
   cJSON_Delete(frame);
   cJSON_Delete(request);
   cJSON_Delete(decision);
   request = cJSON_Parse("{\"tool_name\":\"AskUserQuestion\",\"input\":{\"questions\":[{"
                         "\"question\":\"Pick a model\"}]}}");
   decision = cJSON_Parse("{\"decision\":\"allow\",\"answers\":{\"Pick a model\":[\"model-b\"]}}");
   frame = aimee_claude_control_reply("question", request, decision);
   assert(frame);
   response = cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "response"), "response");
   assert(
       !strcmp(string(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "updatedInput"), "answers"),
                      "Pick a model"),
               "model-b"));
   cJSON_Delete(frame);
   cJSON_Delete(request);
   cJSON_Delete(decision);
   request = cJSON_Parse("{\"questions\":[{\"id\":\"q1\",\"question\":\"Pick a model\"}]}");
   decision = cJSON_Parse("{\"decision\":\"allow\",\"answers\":{\"q1\":[\"model-b\"]}}");
   response = aimee_codex_control_reply("item/tool/requestUserInput", request, decision);
   assert(response);
   assert(!strcmp(
       cJSON_GetStringValue(cJSON_GetArrayItem(
           cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "answers"), "q1"),
                               "answers"),
           0)),
       "model-b"));
   cJSON_Delete(response);
   cJSON_Delete(decision);
   decision = cJSON_Parse("{\"decision\":\"deny\"}");
   response = aimee_codex_control_reply("item/commandExecution/requestApproval", request, decision);
   assert(response);
   assert(!strcmp(string(response, "decision"), "decline"));
   cJSON_Delete(response);
   cJSON_Delete(decision);
   cJSON_Delete(request);
   request =
       cJSON_Parse("{\"mode\":\"form\",\"serverName\":\"inventory\",\"message\":\"Quantity?\","
                   "\"requestedSchema\":{\"type\":\"object\",\"properties\":{\"quantity\":{"
                   "\"type\":\"integer\",\"minimum\":1}},\"required\":[\"quantity\"]}}");
   decision = cJSON_Parse("{\"decision\":\"allow\",\"content\":{\"quantity\":3}}");
   response = aimee_codex_control_reply("mcpServer/elicitation/request", request, decision);
   assert(response && !strcmp(string(response, "action"), "accept"));
   assert(cJSON_Compare(cJSON_GetObjectItem(response, "content"),
                        cJSON_GetObjectItem(decision, "content"), 1));
   cJSON_Delete(response);
   cJSON_Delete(decision);
   decision = cJSON_Parse("{\"decision\":\"allow\",\"content\":{\"quantity\":0}}");
   assert(!aimee_codex_control_reply("mcpServer/elicitation/request", request, decision));
   cJSON_Delete(decision);
   decision = cJSON_Parse("{\"decision\":\"cancel\"}");
   response = aimee_codex_control_reply("mcpServer/elicitation/request", request, decision);
   assert(response && !strcmp(string(response, "action"), "cancel"));
   assert(cJSON_IsNull(cJSON_GetObjectItem(response, "content")));
   cJSON_Delete(response);
   cJSON_Delete(decision);
   cJSON_Delete(request);
}

static void url_elicitation(void)
{
   aimee_harness_ir_t state = {0};
   assert(frame("{\"id\":73,\"method\":\"mcpServer/elicitation/"
                "request\",\"params\":{\"mode\":\"url\",\"serverName\":\"inventory\",\"message\":"
                "\"Authorize "
                "access\",\"url\":\"https://accounts.example.test/"
                "approve?state=opaque\",\"elicitationId\":\"el-1\"}}",
                &state, 1) == 0);
   const cJSON *event = cJSON_GetArrayItem(events, 0);
   const cJSON *request = cJSON_GetObjectItem(event, "payload");
   const cJSON *url = cJSON_GetObjectItem(request, "url");
   assert(!strcmp(string(event, "control_kind"), "elicitation"));
   assert(url && !strcmp(string(url, "elicitation_id"), "el-1") &&
          !strcmp(string(url, "server"), "inventory") && !cJSON_GetObjectItem(request, "input"));
   assert(aimee_ir_elicitation_url_valid(url));
   cJSON *params =
       cJSON_Parse("{\"mode\":\"url\",\"serverName\":\"inventory\",\"message\":\"Authorize "
                   "access\",\"url\":\"https://accounts.example.test/"
                   "approve?state=opaque\",\"elicitationId\":\"el-1\"}");
   cJSON *allow = cJSON_Parse("{\"decision\":\"allow\"}");
   cJSON *reply = aimee_codex_control_reply("mcpServer/elicitation/request", params, allow);
   assert(reply && !strcmp(string(reply, "action"), "accept") &&
          cJSON_IsNull(cJSON_GetObjectItem(reply, "content")));
   cJSON *native_reply = cJSON_CreateObject();
   cJSON_AddNumberToObject(native_reply, "id", 73);
   cJSON_AddItemToObject(native_reply, "result", reply);
   assert(!aimee_codex_reply_ir(native_reply, &state));
   const cJSON *completed = cJSON_GetObjectItem(cJSON_GetArrayItem(events, 1), "payload");
   assert(!strcmp(string(completed, "decision"), "allow") &&
          !cJSON_GetObjectItem(completed, "action"));
   cJSON_Delete(native_reply);
   cJSON_AddObjectToObject(allow, "content");
   assert(!aimee_codex_control_reply("mcpServer/elicitation/request", params, allow));
   cJSON_DeleteItemFromObjectCaseSensitive(allow, "content");
   cJSON_SetValuestring(cJSON_GetObjectItem(params, "url"), "javascript:alert(1)");
   assert(!aimee_codex_control_reply("mcpServer/elicitation/request", params, allow));
   cJSON_SetValuestring(cJSON_GetObjectItem(allow, "decision"), "deny");
   reply = aimee_codex_control_reply("mcpServer/elicitation/request", params, allow);
   assert(reply && !strcmp(string(reply, "action"), "decline"));
   cJSON_Delete(reply);
   cJSON_Delete(allow);
   cJSON_Delete(params);
   aimee_harness_ir_free(&state);
   cJSON_DeleteItemFromArray(events, 0);
   cJSON_DeleteItemFromArray(events, 0);
}

static void native_compactions(void)
{
   aimee_harness_ir_t state = {0};
   const char *frames[] = {
       "{\"method\":\"thread/compacted\",\"params\":{\"threadId\":\"native\"}}",
       "{\"method\":\"item/"
       "completed\",\"params\":{\"item\":{\"id\":\"compact\",\"type\":\"contextCompaction\"}}}",
       "{\"type\":\"system\",\"subtype\":\"compact_boundary\",\"compact_metadata\":{\"trigger\":"
       "\"auto\"}}"};
   for (int i = 0; i < 3; i++)
   {
      int rc = frame(frames[i], &state, i < 2);
      assert(rc == 0 && cJSON_GetArraySize(events) == 1);
      cJSON *event = cJSON_GetArrayItem(events, 0);
      assert(!strcmp(string(event, "kind"), "native_compaction"));
      cJSON *payload = cJSON_GetObjectItemCaseSensitive(event, "payload");
      assert(cJSON_GetArraySize(payload) == 1);
      assert(!strcmp(string(payload, "harness"), i < 2 ? "codex" : "claude"));
      cJSON_DeleteItemFromArray(events, 0);
   }
   reject = 1;
   int rc = frame(frames[0], &state, 1);
   assert(rc < 0 && aimee_ir_session_capture_failed());
   reject = 0;
   next_execution();
   aimee_harness_ir_free(&state);
}

static void owned_results(void)
{
   next_execution();
   aimee_harness_ir_t state = {0};
   aimee_response_t result = {0};
   assert(frame("{\"method\":\"session/"
                "update\",\"params\":{\"update\":{\"sessionUpdate\":\"agent_thought_chunk\","
                "\"content\":{\"type\":\"text\",\"text\":\"Reason\"}}}}",
                &state, 2) == 0);
   assert(aimee_harness_ir_response(&state, &result) < 0 && !result.content);
   assert(frame("{\"method\":\"session/"
                "update\",\"params\":{\"update\":{\"sessionUpdate\":\"agent_message_chunk\","
                "\"content\":{\"type\":\"text\",\"text\":\"Answer\"}}}}",
                &state, 2) == 0);
   assert(frame("{\"method\":\"session/"
                "update\",\"params\":{\"update\":{\"sessionUpdate\":\"agent_message_chunk\","
                "\"content\":{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"AA==\"}}}}",
                &state, 2) == 0);
   assert(frame("{\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}", &state, 2) == 0);
   assert(!aimee_harness_ir_response(&state, &result));
   assert(result.n_content == 3);
   assert(result.content[0].type == AIMEE_BLK_THINKING &&
          !strcmp(result.content[0].text, "Reason"));
   assert(result.content[1].type == AIMEE_BLK_TEXT && !strcmp(result.content[1].text, "Answer"));
   assert(result.content[2].type == AIMEE_BLK_IMAGE &&
          !strcmp(result.content[2].media_ref, "data:image/png;base64,AA=="));
   assert(result.content[0].message_group != result.content[1].message_group);
   aimee_harness_ir_free(&state);
   assert(!strcmp(result.content[0].text, "Reason")); /* Not borrowed from the frame/state. */
   aimee_response_free(&result);

   next_execution();
   assert(frame("{\"id\":3,\"result\":{\"content\":\"legacy native "
                "text\",\"stopReason\":\"max_tokens\"}}",
                &state, 2) == 0);
   assert(!aimee_harness_ir_response(&state, &result));
   aimee_harness_ir_free(&state);
   assert(result.n_content == 1 && !strcmp(result.content[0].text, "legacy native text"));
   assert(result.stop_reason == AIMEE_STOP_MAX_TOKENS);
   assert(!strcmp(result.raw_stop_reason, "max_tokens"));
   aimee_response_free(&result);

   next_execution();
   assert(frame("{\"method\":\"item/agentMessage/"
                "delta\",\"params\":{\"itemId\":\"native-id\",\"delta\":\"prefix\"}}",
                &state, 1) == 0);
   assert(aimee_harness_ir_response(&state, &result) < 0);
   assert(frame("{\"method\":\"item/"
                "completed\",\"params\":{\"item\":{\"id\":\"native-id\",\"type\":\"agentMessage\","
                "\"text\":\"prefix completed\"}}}",
                &state, 1) == 0);
   assert(!aimee_harness_ir_response(&state, &result));
   assert(result.n_content == 1 && !strcmp(result.content[0].text, "prefix completed"));
   assert(!strcmp(result.id, "native-id"));
   /* Repeated immutable snapshots cannot duplicate the display/result. */
   assert(frame("{\"method\":\"item/"
                "completed\",\"params\":{\"item\":{\"id\":\"native-id\",\"type\":\"agentMessage\","
                "\"text\":\"prefix completed\"}}}",
                &state, 1) == 0);
   assert(!aimee_harness_ir_response(&state, &result) && result.n_content == 1);
   aimee_block_t *retained = result.content;
   assert(frame("{\"method\":\"item/"
                "completed\",\"params\":{\"item\":{\"id\":\"native-id\",\"type\":\"agentMessage\","
                "\"text\":\"contradictory\"}}}",
                &state, 1) < 0);
   assert(aimee_ir_session_capture_failed());
   assert(aimee_harness_ir_response(&state, &result) < 0 && result.content == retained);
   aimee_harness_ir_free(&state);
   aimee_response_free(&result);
   next_execution();
}

int main(void)
{
   control_outputs();
   events = cJSON_CreateArray();
   aimee_ir_session_set_sink(capture, NULL);
   owned_results();
   cJSON_Delete(events);
   events = cJSON_CreateArray();
   native_compactions();
   url_elicitation();
   aimee_harness_ir_t form_state = {0};
   assert(
       frame(
           "{\"id\":42,\"method\":\"mcpServer/elicitation/request\",\"params\":{\"mode\":\"form\","
           "\"message\":\"Quantity?\",\"serverName\":\"inventory\",\"requestedSchema\":{\"type\":"
           "\"object\","
           "\"properties\":{\"quantity\":{\"type\":\"integer\"}}}}}",
           &form_state, 1) == 0);
   const cJSON *event = cJSON_GetArrayItem(events, 0);
   assert(!strcmp(string(event, "control_kind"), "elicitation"));
   const cJSON *form = cJSON_GetObjectItem(cJSON_GetObjectItem(event, "payload"), "form");
   assert(form && !strcmp(string(form, "message"), "Quantity?") &&
          !strcmp(string(form, "server"), "inventory"));
   assert(cJSON_IsObject(cJSON_GetObjectItem(form, "schema")));
   aimee_harness_ir_free(&form_state);
   cJSON_DeleteItemFromArray(events, 0);
   claude();
   next_execution();
   claude_block_snapshots();
   next_execution();
   codex();
   next_execution();
   codex_tool_results();
   next_execution();
   acp();
   aimee_ir_session_set_sink(NULL, NULL);
   cJSON_Delete(events);
   puts("harness IR boundary tests passed");
   return 0;
}
