/* Actual HTTP ingress request assembly cannot bypass canonical admission. */
#include "../server/openai_chat.c"
#include <assert.h>
void ir_seam_test_memory(int enabled);
int ir_seam_test_plan_calls(const char *phase);
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
   assert(!aimee_openai_emit_chat_response(&response, "chat-fixture", "model-alias", 123,
                                           collect_chat_frame, NULL));
   assert(chat_frames == 2);
   assert(!aimee_openai_emit_responses_tail(&response, "resp-fixture", "model-alias", 123,
                                            collect_ir_event, NULL));
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
   assert(!aimee_openai_emit_responses_tail(&response, "resp-incomplete", "model-alias", 123,
                                            collect_ir_event, NULL));
   assert(strstr(captured_stream, "event: response.incomplete\n"));
   free(captured_stream);
   captured_stream = NULL;
   captured_length = 0;
   aimee_response_free(&response);
   puts("OpenAI HTTP owned response rendering preserves ordered blocks and 17 full tool calls");
}

static void test_tool_free_admission_and_text_entry(void)
{
   char error[160];
   aimee_request_t request = {0};
   const char *chat =
       "{\"model\":\"named-agent\",\"messages\":["
       "{\"role\":\"system\",\"content\":\"policy\"},"
       "{\"role\":\"user\",\"content\":\"first\"},"
       "{\"role\":\"assistant\",\"content\":\"answer\"},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"last\"},"
       "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.invalid/image.png\"}}]}]}";
   assert(!openai_admit_completion(chat, 1, &request, error, sizeof error));
   assert(request.n_tools == 0 && request.n_system == 1 && request.n_messages == 3);
   assert(!strcmp(request.messages[0].role, "user") &&
          !strcmp(request.messages[1].role, "assistant"));
   assert(request.messages[2].n_blocks == 2 &&
          request.messages[2].blocks[1].type == AIMEE_BLK_IMAGE);
   aimee_request_free(&request);
   const char *text =
       "{\"model\":\"named-agent\",\"prompt\":\"raw prompt\",\"max_tokens\":77,"
       "\"temperature\":0.2,\"top_p\":0.9,\"top_k\":7,\"stream\":true,\"stop\":[\"STOP\",\"END\"]}";
   assert(!openai_admit_completion(text, 0, &request, error, sizeof error));
   assert(!request.raw && !request.raw_bytes && request.n_messages == 1 && request.n_system == 0);
   assert(!strcmp(request.messages[0].blocks[0].text, "raw prompt"));
   assert(request.stream && request.max_tokens == 77 && request.temperature == 0.2 &&
          request.top_p == 0.9 && request.top_k == 7);
   assert(request.n_stop == 2 && !strcmp(request.stop_sequences[1], "END"));
   char *native = openai_build_ir_body(&request, AIMEE_WIRE_ANTHROPIC);
   assert(native && strstr(native, "raw prompt") && strstr(native, "stop_sequences"));
   free(native);
   aimee_request_free(&request);
   const char *invalid[] = {"{\"prompt\":\"x\"} trailing",
                            "{\"prompt\":[]}",
                            "{\"prompt\":\"x\",\"max_tokens\":1.5}",
                            "{\"prompt\":\"x\",\"temperature\":3}",
                            "{\"prompt\":\"x\",\"stop\":[42]}",
                            "{\"prompt\":\"x\",\"tools\":[]}",
                            "{\"prompt\":\"x\",\"n\":2}",
                            "{\"prompt\":\"x\",\"echo\":true}"};
   for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
   {
      assert(openai_admit_completion(invalid[i], 0, &request, error, sizeof error));
      aimee_request_free(&request);
   }
   assert(openai_admit_completion("{\"messages\":[]}", 1, &request, error, sizeof error));
   aimee_request_free(&request);
}
static void test_canonical_response_cache_round_trip(void)
{
   aimee_response_t response = {.role = strdup("assistant"),
                                .id = strdup("native-id"),
                                .model = strdup("native-model"),
                                .stop_reason = AIMEE_STOP_MAX_TOKENS,
                                .raw_stop_reason = strdup("length"),
                                .usage_in = 32,
                                .usage_out = 8,
                                .usage_cache_read = 16,
                                .usage_cache_write = 4,
                                .usage_reasoning = 3,
                                .n_content = 3};
   response.content = calloc(3, sizeof *response.content);
   assert(response.content);
   response.content[0] = (aimee_block_t){.type = AIMEE_BLK_THINKING,
                                         .text = strdup("thought"),
                                         .thinking_signature = strdup("opaque-signature"),
                                         .thinking_encrypted_content = strdup("cipher"),
                                         .thinking_id = strdup("reasoning-id")};
   response.content[1] =
       (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup("answer"), .message_group = 1};
   response.content[2] = (aimee_block_t){.type = AIMEE_BLK_IMAGE,
                                         .media_ref = strdup("https://example.invalid/image.png")};
   response.raw = cJSON_Parse("{\"native_sidecar_must_not_enter_cache\":true}");
   openai_ir_cache_t first = {.created = 123};
   snprintf(first.client_id, sizeof first.client_id, "chatcmpl-stable");
   char *encoded = openai_ir_cache_encode(&response, &first);
   assert(encoded && !strstr(encoded, "native_sidecar_must_not_enter_cache"));
   aimee_response_t restored = {0};
   openai_ir_cache_t replay = {0};
   assert(!openai_ir_cache_decode(encoded, &restored, &replay));
   assert(!strcmp(replay.client_id, first.client_id) && replay.created == first.created);
   assert(restored.n_content == 3 &&
          !strcmp(restored.content[0].thinking_signature, "opaque-signature"));
   assert(!strcmp(restored.content[0].thinking_encrypted_content, "cipher"));
   assert(restored.content[1].message_group == 1 && restored.content[2].type == AIMEE_BLK_IMAGE);
   assert(restored.stop_reason == AIMEE_STOP_MAX_TOKENS &&
          !strcmp(restored.raw_stop_reason, "length"));
   assert(restored.usage_in == 32 && restored.usage_cache_read == 16 &&
          restored.usage_reasoning == 3);
   assert(!strcmp(restored.id, "native-id") && !strcmp(restored.model, "native-model") &&
          !restored.raw);
   aimee_response_free(&restored);
   cJSON *invalid = cJSON_Parse(encoded);
   assert(invalid);
   cJSON_ReplaceItemInObjectCaseSensitive(invalid, "usage_in", cJSON_CreateNumber(-1));
   char *bad = cJSON_PrintUnformatted(invalid);
   assert(openai_ir_cache_decode(bad, &restored, &replay) && !restored.content);
   free(bad);
   cJSON_Delete(invalid);
   free(encoded);
   aimee_response_free(&response);
}

static void test_responses_run_input_controls(void)
{
   aimee_request_t request = {0};
   char error[160];
   const char *body = "{\"input\":[\"first\",{\"role\":\"assistant\",\"content\":\"prior\"},"
                      "{\"role\":\"user\",\"content\":\"last\"}],\"instructions\":\"policy\","
                      "\"temperature\":0.25,\"max_output_tokens\":1024}";
   assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_RESPONSES, &request, error,
                                     sizeof error));
   assert(request.n_messages == 3 && request.n_system == 1);
   assert(!strcmp(request.messages[0].role, "user"));
   assert(!strcmp(request.messages[0].blocks[0].text, "first"));
   assert(!strcmp(request.messages[1].role, "assistant"));
   assert(request.has_temperature && request.temperature == 0.25);
   assert(request.has_max_tokens && request.max_tokens == 1024);
   aimee_request_free(&request);
   const char *invalid[] = {
       "{\"input\":\"x\",\"temperature\":3}", "{\"input\":\"x\",\"temperature\":\"0.3\"}",
       "{\"input\":\"x\",\"max_output_tokens\":1.5}", "{\"input\":\"x\",\"max_output_tokens\":0}"};
   for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
   {
      assert(aimee_frontend_parse_wire(invalid[i], strlen(invalid[i]), AIMEE_WIRE_RESPONSES,
                                       &request, error, sizeof error));
      aimee_request_free(&request);
   }
}

int main(void)
{
   test_responses_run_input_controls();
   test_tool_free_admission_and_text_entry();
   test_canonical_response_cache_round_trip();
   test_owned_http_response_boundaries();
   const char *body =
       "{\"model\":\"fixture-model\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}],"
       "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"Read\",\"parameters\":{\"type\":"
       "\"object\"},\"strict\":true}}],"
       "\"reasoning_effort\":\"high\",\"tool_choice\":{\"type\":\"function\",\"function\":{"
       "\"name\":\"Read\"}}} ";
   aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_ANTHROPIC, AIMEE_WIRE_RESPONSES};
   setenv("AIMEE_IR_PATH", "0", 1);
   for (int preserve = 0; preserve <= 1; preserve++)
      for (size_t i = 0; i < sizeof wires / sizeof wires[0]; i++)
      {
         aimee_request_t canonical = {0};
         char error[128];
         assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_OPENAI_CHAT, &canonical,
                                           error, sizeof error));
         ir_seam_test_memory(1);
         int before = ir_seam_test_plan_calls("context");
         if (!preserve)
            aimee_ir_apply_gateway_request_stages(&canonical);
         char *native = openai_build_ir_body(&canonical, wires[i]);
         assert(native);
         cJSON *request = cJSON_Parse(native);
         assert(request && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "model")),
                                   "fixture-model"));
         assert(ir_seam_test_plan_calls("context") == before + !preserve);
         assert(cJSON_IsArray(cJSON_GetObjectItem(request, i == 2 ? "input" : "messages")));
         cJSON_Delete(request);
         free(native);
         free(canonical.messages[0].role);
         canonical.messages[0].role = strdup("forged-role");
         assert(!openai_build_ir_body(&canonical, wires[i]));
         aimee_request_free(&canonical);
      }
   puts("OpenAI ingress canonical admission and independent host-edit policy passed");
   return 0;
}
