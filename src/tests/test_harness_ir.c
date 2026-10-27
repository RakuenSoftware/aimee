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
   int rc = codex ? aimee_codex_frame_ir(f, s) : aimee_claude_frame_ir(f, s);
   cJSON_Delete(f);
   return rc;
}
static const char *string(const cJSON *o, const char *name)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, name));
}
/* The original native gateway owns all model/history/tool observations.
 * This adapter must never synthesize a competing transcript, in any mode. */
static void native_capture_owner(void)
{
   const char *claude_frames[] = {
       "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\",\"message\":{\"id\":\"m\","
       "\"role\":\"assistant\"}}}",
       "{\"type\":\"assistant\",\"message\":{\"id\":\"m\",\"role\":\"assistant\",\"content\":[{"
       "\"type\":\"text\",\"text\":\"PRIVATE_MODEL_TEXT\"}]}}",
       "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\","
       "\"tool_use_id\":\"call\",\"content\":\"PRIVATE_TOOL_RESULT\"}]}}",
       "{\"type\":\"result\",\"is_error\":false,\"result\":\"PRIVATE_RESULT\"}"};
   aimee_harness_ir_t state = {0};
   int before = cJSON_GetArraySize(events);
   for (size_t i = 0; i < sizeof claude_frames / sizeof *claude_frames; i++)
   {
      assert(!frame(claude_frames[i], &state, 0));
      assert(cJSON_GetArraySize(events) == before);
   }
   const char *types[] = {"agentMessage",     "userMessage", "reasoning",       "plan",
                          "commandExecution", "mcpToolCall", "dynamicToolCall", "fileChange",
                          "webSearch",        "imageView"};
   for (size_t i = 0; i < sizeof types / sizeof *types; i++)
      for (int complete = 0; complete < 2; complete++)
      {
         char raw[512];
         snprintf(raw, sizeof raw,
                  "{\"method\":\"item/"
                  "%s\",\"params\":{\"item\":{\"id\":\"m\",\"type\":\"%s\",\"text\":\"PRIVATE_"
                  "MODEL_TEXT\"}}}",
                  complete ? "completed" : "started", types[i]);
         assert(!frame(raw, &state, 1));
         assert(cJSON_GetArraySize(events) == before);
      }
   assert(!frame("{\"method\":\"item/agentMessage/"
                 "delta\",\"params\":{\"itemId\":\"m\",\"delta\":\"PRIVATE_DELTA\"}}",
                 &state, 1));
   assert(cJSON_GetArraySize(events) == before);
   /* Incoming controls still persist once and resolve in their own ID space. */
   assert(!frame("{\"id\":7,\"method\":\"item/commandExecution/"
                 "requestApproval\",\"params\":{\"command\":\"pwd\"}}",
                 &state, 1));
   assert(cJSON_GetArraySize(events) == before + 1);
   cJSON *reply = cJSON_Parse("{\"id\":7,\"result\":{\"decision\":\"accept\"}}");
   assert(reply && !aimee_codex_reply_ir(reply, &state));
   assert(cJSON_GetArraySize(events) == before + 2);
   assert(!strcmp(string(cJSON_GetArrayItem(events, before + 1), "kind"), "control_response"));
   assert(!aimee_codex_reply_ir(reply, &state));
   assert(cJSON_GetArraySize(events) == before + 2);
   cJSON_Delete(reply);
   assert(frame("{\"method\":\"item/"
                "completed\",\"params\":{\"item\":{\"id\":\"unknown\",\"type\":\"futureItem\"}}}",
                &state, 1) < 0);
   aimee_harness_ir_free(&state);
   next_execution();
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

/* SDK 2.1.177 stdout serializes the reserved error field without a flag.
 * Unknown is observed in a synthetic local HTTP400 control; known values below
 * are constructed source-contract cases, not a claim about a live failure. */
static void claude_legacy_diagnostic(void)
{
   const char *codes[] = {"unknown", "rate_limit", "invalid_request"};
   int before = cJSON_GetArraySize(events);
   for (size_t i = 0; i < sizeof codes / sizeof *codes; i++)
   {
      aimee_harness_ir_t state = {0};
      char raw[1024];
      snprintf(raw, sizeof raw,
               "{\"type\":\"assistant\",\"error\":\"%s\",\"session_id\":\"native-one\","
               "\"uuid\":\"sdk-uuid\",\"parent_tool_use_id\":null,\"message\":{"
               "\"id\":\"sdk-diagnostic\",\"role\":\"assistant\",\"content\":[{"
               "\"type\":\"text\",\"text\":\"PRIVATE_DIAGNOSTIC\"}]}}",
               codes[i]);
      assert(!frame(raw, &state, 0));
      assert(cJSON_GetArraySize(events) == before);
      assert(state.failure == (i == 1   ? AIMEE_HARNESS_FAILURE_RATE_LIMIT
                               : i == 2 ? AIMEE_HARNESS_FAILURE_PROVIDER_REJECTED
                                        : AIMEE_HARNESS_FAILURE_NONE));
      /* Each malformed case changes exactly one otherwise complete field. */
      if (i == 0)
      {
         for (int bad_parent = 0; bad_parent < 2; bad_parent++)
         {
            cJSON *bad = cJSON_Parse(raw);
            assert(bad);
            if (bad_parent)
               assert(cJSON_ReplaceItemInObjectCaseSensitive(bad, "parent_tool_use_id",
                                                             cJSON_CreateString("subagent-tool")));
            else
               assert(cJSON_ReplaceItemInObjectCaseSensitive(
                   cJSON_GetObjectItemCaseSensitive(bad, "message"), "role",
                   cJSON_CreateString("user")));
            char *malformed = cJSON_PrintUnformatted(bad);
            assert(malformed && frame(malformed, &state, 0) < 0);
            assert(state.failure == AIMEE_HARNESS_FAILURE_NONE &&
                   cJSON_GetArraySize(events) == before);
            free(malformed);
            cJSON_Delete(bad);
            next_execution();
         }
      }
      aimee_harness_ir_free(&state);
   }
   /* A non-diagnostic assistant may carry unrelated SDK metadata. */
   aimee_harness_ir_t normal = {0};
   assert(!frame("{\"type\":\"assistant\",\"is_api_error_message\":false,"
                 "\"future_metadata\":true,\"message\":{\"role\":\"assistant\","
                 "\"content\":[{\"type\":\"text\",\"text\":\"invalid_request\"}]}}",
                 &normal, 0));
   assert(normal.failure == AIMEE_HARNESS_FAILURE_NONE && cJSON_GetArraySize(events) == before);
   aimee_harness_ir_free(&normal);
}
static void claude_failure_classification(void)
{
   const char *codes[] = {"rate_limit", "invalid_request", "unknown_new_code",
                          "PRIVATE_PROVIDER_DIAGNOSTIC"};
   int before = cJSON_GetArraySize(events);
   {
      aimee_harness_ir_t state = {0};
      for (unsigned i = 0; i < sizeof codes / sizeof *codes; i++)
      {
         char raw[1024];
         snprintf(raw, sizeof raw,
                  "{\"type\":\"assistant\",\"is_api_error_message\":true,\"error\":\"%s\","
                  "\"message\":{\"id\":\"sdk-api-error\",\"role\":\"assistant\",\"content\":["
                  "{\"type\":\"text\",\"text\":\"PRIVATE_PROVIDER_DIAGNOSTIC\"}]}}",
                  codes[i]);
         assert(!frame(raw, &state, 0));
         assert(state.failure == (i == 0   ? AIMEE_HARNESS_FAILURE_RATE_LIMIT
                                  : i == 1 ? AIMEE_HARNESS_FAILURE_PROVIDER_REJECTED
                                           : AIMEE_HARNESS_FAILURE_NONE));
         assert(cJSON_GetArraySize(events) == before);
      }
      assert(
          !frame("{\"type\":\"assistant\",\"is_api_error_message\":true,\"error\":\"rate_limit\","
                 "\"message\":{\"role\":\"assistant\"}}",
                 &state, 0));
      assert(state.failure == AIMEE_HARNESS_FAILURE_RATE_LIMIT);
      assert(!frame("{\"type\":\"result\",\"is_error\":false}", &state, 0));
      assert(state.failure == AIMEE_HARNESS_FAILURE_NONE);
      aimee_harness_ir_free(&state);
   }
   /* Ordinary assistant content and non-boolean lookalikes cannot classify. */
   aimee_harness_ir_t state = {0};
   assert(
       frame("{\"type\":\"assistant\",\"is_api_error_message\":\"true\",\"error\":\"rate_limit\","
             "\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\","
             "\"text\":\"rate_limit\"}]}}",
             &state, 0) < 0);
   next_execution();
   assert(state.failure == AIMEE_HARNESS_FAILURE_NONE);
   assert(
       frame(
           "{\"type\":\"assistant\",\"error\":\"rate_limit\",\"message\":{\"role\":\"assistant\"}}",
           &state, 0) < 0);
   next_execution();
   /* Camel case belongs to the internal conversation log, not print stdout. */
   assert(frame("{\"type\":\"assistant\",\"isApiErrorMessage\":true,\"error\":\"rate_limit\","
                "\"message\":{\"role\":\"assistant\"}}",
                &state, 0) < 0);
   next_execution();
   assert(state.failure == AIMEE_HARNESS_FAILURE_NONE && cJSON_GetArraySize(events) == before);
   /* Provider text and result errors are not SDK API classification authority. */
   assert(frame("{\"type\":\"assistant\",\"is_api_error_message\":false,"
                "\"error\":\"invalid_request\",\"message\":{\"role\":\"assistant\"}}",
                &state, 0) < 0);
   next_execution();
   assert(state.failure == AIMEE_HARNESS_FAILURE_NONE);
   assert(!frame("{\"type\":\"result\",\"is_error\":true,"
                 "\"errors\":[\"invalid_request\",\"PRIVATE_PROVIDER_DIAGNOSTIC\"]}",
                 &state, 0));
   assert(state.failure == AIMEE_HARNESS_FAILURE_NONE && cJSON_GetArraySize(events) == before);
   aimee_harness_ir_free(&state);
}
int main(void)
{
   control_outputs();
   events = cJSON_CreateArray();
   aimee_ir_session_set_sink(capture, NULL);
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
   native_capture_owner();
   next_execution();
   claude_legacy_diagnostic();
   claude_failure_classification();
   next_execution();
   aimee_ir_session_set_sink(NULL, NULL);
   cJSON_Delete(events);
   puts("harness IR boundary tests passed");
   return 0;
}
