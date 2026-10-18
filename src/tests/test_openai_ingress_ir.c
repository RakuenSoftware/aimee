/* Actual HTTP ingress request assembly cannot bypass canonical admission. */
#include "../server/openai_chat.c"
#include <assert.h>
void ir_seam_test_memory(int enabled);
int ir_seam_test_plan_calls(const char *phase);
static cJSON *forbidden_builder(const agent_t *agent, cJSON *messages, cJSON *tools,
                                const char *system, int tokens, double temperature)
{
   (void)agent;
   (void)messages;
   (void)tools;
   (void)system;
   (void)tokens;
   (void)temperature;
   assert(!"legacy request builder must never run");
   return NULL;
}

static char *captured_stream;
static size_t captured_length;
static int added_items, done_items, tool_deltas, terminal_events, chat_frames;
static void append_stream(const char *event, const char *json)
{
   size_t extra = strlen(event) + strlen(json) + 20;
   char *next = realloc(captured_stream, captured_length + extra);
   assert(next);
   captured_stream = next;
   int length = snprintf(next + captured_length, extra, "event: %s\ndata: %s\n\n", event, json);
   assert(length > 0 && (size_t)length < extra);
   captured_length += (size_t)length;
}
static void collect_ir_event(void *context, const char *event, const char *json)
{
   (void)context;
   cJSON *root = cJSON_Parse(json);
   assert(root);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "type")), event));
   if (!strcmp(event, "response.output_item.added"))
      added_items++;
   if (!strcmp(event, "response.output_item.done"))
      done_items++;
   if (!strcmp(event, "response.function_call_arguments.delta"))
   {
      assert(added_items > done_items);
      tool_deltas++;
   }
   if (!strcmp(event, "response.completed") || !strcmp(event, "response.incomplete"))
   {
      terminal_events++;
      assert(added_items == done_items);
   }
   cJSON_Delete(root);
   append_stream(event, json);
}
static void collect_chat_frame(void *context, const char *json)
{
   (void)context;
   cJSON *root = cJSON_Parse(json);
   assert(root);
   cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
   cJSON *delta = cJSON_GetObjectItem(choice, "delta");
   if (!chat_frames)
   {
      cJSON *calls = cJSON_GetObjectItem(delta, "tool_calls");
      assert(cJSON_GetArraySize(calls) == 17);
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(delta, "reasoning_content")),
                     "private thought"));
      for (int i = 0; i < 17; i++)
      {
         cJSON *call = cJSON_GetArrayItem(calls, i);
         assert(cJSON_GetNumberValue(cJSON_GetObjectItem(call, "index")) == i);
         assert(strlen(cJSON_GetStringValue(cJSON_GetObjectItem(call, "id"))) == 511);
      }
   }
   else
   {
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(choice, "finish_reason")),
                     "tool_calls"));
      assert(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "usage"),
                                                      "completion_tokens")) == 9);
   }
   chat_frames++;
   cJSON_Delete(root);
}
static void test_owned_http_response_boundaries(void)
{
   aimee_response_t response = {.role = strdup("assistant"),
                                .stop_reason = AIMEE_STOP_TOOL_USE,
                                .usage_in = 42,
                                .usage_out = 9,
                                .usage_cache_read = 17,
                                .usage_reasoning = 3,
                                .n_content = 21};
   response.content = calloc((size_t)response.n_content, sizeof *response.content);
   assert(response.content);
   response.content[0] = (aimee_block_t){.type = AIMEE_BLK_THINKING,
                                         .text = strdup("private thought"),
                                         .thinking_id = strdup("reason-native"),
                                         .thinking_encrypted_content = strdup("opaque-native")};
   response.content[1] =
       (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup("before"), .message_group = 1};
   response.content[2] =
       (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup("after"), .message_group = 1};
   response.content[3] = (aimee_block_t){
       .type = AIMEE_BLK_TEXT, .text = strdup("another message"), .message_group = 2};
   char id[512], payload[8193];
   memset(id, 'i', sizeof id - 1);
   id[sizeof id - 1] = 0;
   memset(payload, 'p', sizeof payload - 1);
   payload[sizeof payload - 1] = 0;
   for (int i = 0; i < 17; i++)
   {
      id[0] = (char)('a' + i);
      aimee_block_t *block = &response.content[4 + i];
      *block = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                               .tool_id = strdup(id),
                               .tool_namespace = strdup("mcp__aimee"),
                               .tool_name = strdup("read"),
                               .tool_input = cJSON_CreateObject()};
      cJSON_AddStringToObject(block->tool_input, "payload", payload);
   }
   assert(!aimee_ir_response_admit(&response));
   assert(!openai_ir_chat_emit(&response, "chat-fixture", "model-alias", 123, collect_chat_frame,
                               NULL));
   assert(chat_frames == 2);
   assert(!openai_ir_responses_emit(&response, "resp-fixture", "model-alias", 123, collect_ir_event,
                                    NULL));
   assert(added_items == 20 && done_items == 20 && tool_deltas == 17 && terminal_events == 1);
   aimee_response_t replay = {0};
   char error[128];
   assert(!aimee_backend_parse_wire(captured_stream, captured_length, AIMEE_WIRE_RESPONSES, &replay,
                                    error, sizeof error));
   assert(replay.n_content == response.n_content && replay.usage_in == 42 && replay.usage_out == 9);
   assert(replay.usage_cache_read == 17 && replay.usage_reasoning == 3);
   assert(!strcmp(replay.content[0].thinking_encrypted_content, "opaque-native"));
   for (int i = 0; i < 17; i++)
   {
      const aimee_block_t *block = &replay.content[4 + i];
      assert(!strcmp(block->tool_id, response.content[4 + i].tool_id));
      assert(!strcmp(block->tool_namespace, "mcp__aimee"));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(block->tool_input, "payload")),
                     payload));
   }
   aimee_response_free(&replay);
   free(captured_stream);
   captured_stream = NULL;
   captured_length = 0;
   response.stop_reason = AIMEE_STOP_MAX_TOKENS;
   added_items = done_items = tool_deltas = terminal_events = 0;
   assert(!openai_ir_responses_emit(&response, "resp-incomplete", "model-alias", 123,
                                    collect_ir_event, NULL));
   assert(strstr(captured_stream, "event: response.incomplete\n"));
   free(captured_stream);
   captured_stream = NULL;
   captured_length = 0;
   aimee_response_free(&response);
   puts("OpenAI HTTP owned response rendering preserves ordered blocks and 17 full tool calls");
}

int main(void)
{
   test_owned_http_response_boundaries();
   agent_t agent = {0};
   snprintf(agent.model, sizeof agent.model, "fixture-model");
   cJSON *messages = cJSON_Parse("[{\"role\":\"user\",\"content\":\"hello\"}]");
   cJSON *tools = cJSON_Parse("[{\"type\":\"function\",\"function\":{\"name\":\"Read\","
                              "\"parameters\":{\"type\":\"object\"}}}]");
   const char *drivers[] = {"openai", "anthropic", "chatgpt"};
   setenv("AIMEE_IR_PATH", "0", 1);
   for (int preserve = 0; preserve <= 1; preserve++)
      for (size_t i = 0; i < sizeof drivers / sizeof drivers[0]; i++)
      {
         delegate_driver_t driver = {.name = drivers[i], .build_request = forbidden_builder};
         ir_seam_test_memory(1);
         int before = ir_seam_test_plan_calls("context");
         char *body =
             openai_build_body(&agent, &driver, messages, tools, "policy", 17, 0.2, preserve);
         assert(body);
         cJSON *request = cJSON_Parse(body);
         assert(request &&
                !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "model")), agent.model));
         assert(ir_seam_test_plan_calls("context") == before + !preserve);
         assert(cJSON_IsArray(cJSON_GetObjectItem(request, i == 2 ? "input" : "messages")));
         cJSON_Delete(request);
         free(body);
         /* An invalid tool schema is a refusal, never a driver fallback. */
         cJSON *bad = cJSON_Parse("[{\"type\":\"unsupported_tool\"}]");
         assert(!openai_build_body(&agent, &driver, messages, bad, "policy", 17, 0.2, preserve));
         cJSON_Delete(bad);
         bad = cJSON_Parse("[{\"role\":\"forged-role\",\"content\":\"hidden\"}]");
         assert(!openai_build_body(&agent, &driver, bad, tools, "policy", 17, 0.2, preserve));
         cJSON_Delete(bad);
         bad = cJSON_Parse("[{\"role\":\"user\",\"content\":[{\"type\":\"unknown-content\"}]}]");
         assert(!openai_build_body(&agent, &driver, bad, tools, "policy", 17, 0.2, preserve));
         cJSON_Delete(bad);
      }
   cJSON_Delete(messages);
   cJSON_Delete(tools);
   puts("OpenAI ingress canonical admission and independent host-edit policy passed");
   return 0;
}
