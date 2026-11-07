/* Actual HTTP ingress request assembly cannot bypass canonical admission. */
#include "../server/openai_chat.c"
#include <assert.h>
#include <stdatomic.h>
static atomic_int shadow_deliveries;
static char shadow_body[2048];
int agent_http_post(const char *url, const char *auth, const char *body, char **response,
                    int timeout, const char *headers)
{
   (void)url;
   (void)auth;
   (void)timeout;
   assert(headers && strstr(headers, "X-Aimee-Shadow"));
   snprintf(shadow_body, sizeof shadow_body, "%s", body);
   *response = NULL;
   atomic_fetch_add_explicit(&shadow_deliveries, 1, memory_order_release);
   return 200;
}
int agent_request_max_tokens(const agent_t *agent, int requested)
{
   (void)agent;
   return requested > 0 ? requested : 64;
}
int gateway_policy_apply_ir(aimee_request_t *request)
{
   (void)request;
   return 0;
}
void router_advise_ir(const aimee_request_t *request)
{
   (void)request;
}
char *gw_ir_request_tool_system_prompt(const aimee_request_t *request, const char *base)
{
   (void)request;
   (void)base;
   return strdup("LOCAL_ONLY_POLICY");
}
char *gw_ir_request_completion_system_prompt(const aimee_request_t *request, const char *base)
{
   (void)request;
   (void)base;
   return NULL;
}
static int avoided_audits;
static double avoided_cost;
const request_context_t *request_context_get(void)
{
   return NULL;
}
const char *session_id(void)
{
   return "cache-usage-test";
}
int db1_token_audit_insert(const db1_token_audit_row_t *row)
{
   avoided_audits++;
   avoided_cost = row->estimated_cost_usd;
   return 0;
}

static int actual_audits;
static char actual_usage_kind[24];
int agent_ingress_accounting_enabled(void)
{
   return 1;
}
void agent_ingress_record_cost(const char *name, const char *served, const char *requested,
                               const char *stop, int input, int output, int cache_write,
                               int cache_read, const char *source, const char *kind)
{
   assert(!strcmp(name, "usage-agent") && !strcmp(source, "openai-ingress"));
   assert(!input && !output && !cache_write && !cache_read);
   snprintf(actual_usage_kind, sizeof actual_usage_kind, "%s", kind ? kind : "realized");
   actual_audits++;
}
static void test_actual_ingress_usage_accounting(void)
{
   agent_t agent = {0};
   strcpy(agent.name, "usage-agent");
   for (int unknown = 0; unknown <= 1; unknown++)
   {
      aimee_response_t response = {.usage_unobserved = unknown};
      actual_audits = 0;
      openai_account_response(&agent, "fixture", &response);
      assert(actual_audits == 1);
      assert(!strcmp(actual_usage_kind, unknown ? "unobserved" : "realized"));
   }
}

static void test_cached_unknown_usage(void)
{
   for (int unknown = 0; unknown <= 1; unknown++)
   {
      aimee_response_t response = {.role = strdup("assistant"),
                                   .stop_reason = AIMEE_STOP_END_TURN,
                                   .usage_unobserved = unknown,
                                   .n_content = 1};
      response.content = calloc(1, sizeof *response.content);
      response.content[0] =
          (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup("cached answer")};
      openai_ir_cache_t cache = {.created = 123};
      snprintf(cache.client_id, sizeof cache.client_id, "chatcmpl-usage");
      char *encoded = openai_ir_cache_encode(&response, &cache);
      aimee_response_t restored = {0};
      assert(encoded && !openai_ir_cache_decode(encoded, &restored, &cache));
      assert(restored.usage_unobserved == unknown);
      assert(!restored.usage_in && !restored.usage_out);
      assert(!strcmp(restored.content[0].text, "cached answer"));
      aimee_response_free(&restored);
      /* Old cache rows cannot establish whether numeric zero was observed. */
      cJSON *wire = cJSON_Parse(encoded);
      assert(wire);
      cJSON_DeleteItemFromObjectCaseSensitive(wire, "usage_unobserved");
      char *ambiguous = cJSON_PrintUnformatted(wire);
      assert(ambiguous && openai_ir_cache_decode(ambiguous, &restored, &cache) < 0);
      assert(!restored.content);
      free(ambiguous);
      cJSON *invalid[] = {cJSON_CreateNull(), cJSON_CreateNumber(0), cJSON_CreateString("false")};
      for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
      {
         cJSON_DeleteItemFromObjectCaseSensitive(wire, "usage_unobserved");
         assert(cJSON_AddItemToObject(wire, "usage_unobserved", invalid[i]));
         ambiguous = cJSON_PrintUnformatted(wire);
         assert(ambiguous && openai_ir_cache_decode(ambiguous, &restored, &cache) < 0);
         assert(!restored.content);
         free(ambiguous);
      }
      cJSON_Delete(wire);
      free(encoded);
      aimee_response_free(&response);
   }
   avoided_audits = 0;
   record_avoided_turn("unknown", NAN);
   assert(!avoided_audits);
   record_avoided_turn("observed-zero", 0.0);
   assert(avoided_audits == 1 && avoided_cost == 0.0);
}

static void test_actual_admission_preparation_mirror(void)
{
   shadow_mirror_reset();
   shadow_mirror_set_armed(1);
   assert(!shadow_mirror_subscribe("https://shadow.invalid", "dummy"));
   const char *bad[] = {"{}", "{\"model\":\"a\",\"model\":\"b\",\"messages\":[]}",
                        "{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"tool_choice\":{"
                        "\"type\":\"unknown\"}}"};
   for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
   {
      shadow_mirror_begin_request("/v1/chat/completions", 0);
      aimee_request_t request = {0};
      char error[128];
      assert(openai_admit_completion(bad[i], 1, &request, error, sizeof error));
      shadow_mirror_dispatch_admitted(&request);
      shadow_mirror_end_request();
      aimee_request_free(&request);
   }
   assert(!atomic_load(&shadow_deliveries));
   const char *body =
       "{\"model\":\"incoming-alias\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
   shadow_mirror_begin_request("/v1/chat/completions", 0);
   aimee_request_t request = {0};
   char error[128];
   assert(!openai_admit_completion(body, 1, &request, error, sizeof error));
   agent_t selected = {0};
   snprintf(selected.model, sizeof selected.model, "%s", "local-model");
   assert(!openai_prepare_ir_request(&request, &selected, 64, .3, 0));
   for (int i = 0; i < 200 && !atomic_load_explicit(&shadow_deliveries, memory_order_acquire); i++)
      usleep(10000);
   assert(atomic_load(&shadow_deliveries) == 1);
   assert(!strcmp(shadow_body, body));
   assert(request.n_system && strstr(request.system[0].text, "LOCAL_ONLY_POLICY"));
   shadow_mirror_dispatch_admitted(&request);
   shadow_mirror_end_request();
   aimee_request_free(&request);
   shadow_mirror_reset();
}
void ir_seam_test_memory(int enabled);
int ir_seam_test_plan_calls(const char *phase);
/* This fixture has no stored previous-response checkpoint. Owned profiles
 * must not ask the legacy Responses continuation owner for SDK history. */
const char *server_http_identity_principal(void)
{
   return "fixture-owner";
}
int db1_session_ir_history_load(const char *session, const char *scope, const char *actor,
                                const char *turn, char **out)
{
   (void)session;
   (void)scope;
   (void)actor;
   (void)turn;
   (void)out;
   assert(0 && "unexpected SDK history lookup");
   return -1;
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

static int host_entry_profile(aimee_request_t *request, void *context)
{
   return aimee_ir_request_clone(context, request);
}
static void test_owned_chat_admission_uses_canonical_history(void)
{
   aimee_block_t text = {.type = AIMEE_BLK_TEXT, .text = "owned running history"};
   aimee_message_t message = {.role = "user", .blocks = &text, .n_blocks = 1};
   aimee_request_t profile = {
       .model = "host-model", .messages = &message, .n_messages = 1, .mutated = 1};
   aimee_frontend_state_scope_t previous =
       aimee_frontend_bind_request_owner(host_entry_profile, &profile);
   aimee_request_t admitted = {0};
   char error[160];
   int rc = openai_admit_completion(
       "{\"model\":\"sdk-model\",\"messages\":[{\"role\":\"user\",\"content\":\"SDK history\"}]}",
       1, &admitted, error, sizeof error);
   assert(!rc && !admitted.raw && !admitted.raw_bytes && admitted.n_messages == 1);
   assert(!strcmp(admitted.model, "host-model") &&
          !strcmp(admitted.messages[0].blocks[0].text, "owned running history"));
   aimee_request_free(&admitted);
   rc = responses_admit_request("{\"model\":\"sdk-model\",\"input\":\"SDK history\"}", &admitted,
                                error, sizeof error);
   assert(!rc && !admitted.raw && !admitted.raw_bytes && admitted.n_messages == 1);
   assert(!strcmp(admitted.model, "host-model") &&
          !strcmp(admitted.messages[0].blocks[0].text, "owned running history"));
   aimee_request_free(&admitted);
   assert(responses_admit_request("{\"model\":\"sdk-model\",\"input\":42}", &admitted, error,
                                  sizeof error));
   aimee_frontend_bind_request_owner(previous.guard, previous.context);
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
   assert(request.raw && !request.raw_bytes && request.frontend == AIMEE_WIRE_UNKNOWN &&
          request.n_messages == 1 && request.n_system == 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request.raw, "prompt")), "raw prompt"));
   aimee_request_t clone = {0}, decoded = {0};
   assert(!aimee_ir_request_clone(&request, &clone));
   assert(clone.raw != request.raw && cJSON_Compare(clone.raw, request.raw, 1));
   cJSON *envelope = aimee_ir_request_json(&request);
   assert(envelope && !aimee_ir_request_parse(envelope, &decoded));
   /* Public canonical envelopes intentionally omit whole-wire private
    * sidecars; every modeled legacy control still survives unchanged. */
   assert(!decoded.raw && !decoded.raw_bytes && aimee_ir_request_wire_equal(&decoded, &request));
   cJSON_Delete(envelope);
   aimee_request_free(&decoded);
   aimee_request_free(&clone);
   assert(!strcmp(request.messages[0].blocks[0].text, "raw prompt"));
   assert(request.stream && request.max_tokens == 77 && request.temperature == 0.2 &&
          request.top_p == 0.9 && request.top_k == 7);
   assert(request.n_stop == 2 && !strcmp(request.stop_sequences[1], "END"));
   char *native = agent_render_gateway_request_ir(NULL, &request, AIMEE_WIRE_ANTHROPIC);
   assert(native && strstr(native, "raw prompt") && strstr(native, "stop_sequences"));
   free(native);
   aimee_request_free(&request);
   const char *invalid[] = {"{\"prompt\":\"before\\u0000after\"}",
                            "{\"prompt\":\"first\",\"prompt\":\"second\"}",
                            "{\"prompt\":\"x\"} trailing",
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
   int cache_result = openai_ir_cache_decode(encoded, &restored, &replay);
   assert(!cache_result);
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
   cache_result = openai_ir_cache_decode(bad, &restored, &replay);
   assert(cache_result && !restored.content);
   free(bad);
   cJSON_Delete(invalid);
   free(encoded);
   response.stop_reason = AIMEE_STOP_CONTENT_FILTER;
   free(response.raw_stop_reason);
   response.raw_stop_reason = strdup("refusal");
   aimee_refusal_details_t details = {.category = "future_category", .explanation = NULL};
   response.refusal = aimee_refusal_details_clone(&details);
   assert(response.refusal);
   encoded = openai_ir_cache_encode(&response, &first);
   assert(encoded && !openai_ir_cache_decode(encoded, &restored, &replay));
   assert(restored.refusal && !strcmp(restored.refusal->category, "future_category") &&
          !restored.refusal->explanation);
   aimee_response_free(&restored);
   cJSON *refused = cJSON_Parse(encoded);
   cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(refused, "refusal"),
                           "fallback_credit_token", "never retained");
   char *malformed = cJSON_PrintUnformatted(refused);
   assert(openai_ir_cache_decode(malformed, &restored, &replay) < 0 && !restored.refusal);
   free(malformed);
   cJSON_Delete(refused);
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

static int replay_guard_calls;
static int discard_unproved_replay(aimee_request_t *request, void *deny)
{
   replay_guard_calls++;
   if (deny)
      return -1;
   for (int i = 0; i < request->n_messages; i++)
      for (int j = 0; j < request->messages[i].n_blocks; j++)
      {
         aimee_block_t *b = &request->messages[i].blocks[j];
         free(b->thinking_id);
         free(b->thinking_encrypted_content);
         b->thinking_id = b->thinking_encrypted_content = NULL;
      }
   return 0;
}
static void test_restored_history_rechecks_native_provenance(void)
{
   const char *body = "{\"model\":\"m\",\"input\":\"next\"}";
   aimee_frontend_state_scope_t previous =
       aimee_frontend_bind_state_guard(discard_unproved_replay, NULL);
   aimee_request_t request = {0};
   char error[128];
   assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_RESPONSES, &request, error,
                                     sizeof error));
   assert(replay_guard_calls == 1);
   /* The same owned history replacement used by previous_response_id/economizer
    * restoration happens after native entry and must not bypass the guard. */
   cJSON *history = cJSON_Parse(
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"thinking\",\"text\":\"summary\","
       "\"thinking_id\":\"rs_previous_account\",\"encrypted_content\":\"previous-opaque\"}]},{"
       "\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"next\"}]}]");
   assert(history && !aimee_ir_request_history_replace(&request, history));
   cJSON_Delete(history);
   char *wire = agent_render_gateway_request_ir(NULL, &request, AIMEE_WIRE_RESPONSES);
   assert(wire && !strstr(wire, "rs_previous_account") && !strstr(wire, "previous-opaque"));
   assert(replay_guard_calls == 2);
   free(wire);
   aimee_request_free(&request);
   assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_RESPONSES, &request, error,
                                     sizeof error));
   aimee_frontend_bind_state_guard(discard_unproved_replay, &request);
   assert(!agent_render_gateway_request_ir(NULL, &request, AIMEE_WIRE_RESPONSES) &&
          !request.messages && !request.model);
   aimee_request_free(&request);
   aimee_frontend_bind_state_guard(previous.guard, previous.context);
}

static int cache_projection_capture(const aimee_session_event_t *event, void *context)
{
   int *state = context;
   assert(event->kind == AIMEE_SESSION_PROJECTION &&
          ((event->projection_cache_controls == 3 && !event->projection_request_controls) ||
           (!event->projection_cache_controls && (event->projection_request_controls == 1 ||
                                                  event->projection_request_controls == 3))) &&
          event->projection_reasoning == 0 && event->projection_opaque == 0);
   state[0]++;
   return state[1];
}
static void test_cache_configuration_at_actual_ingress(void)
{
   const char *body =
       "{\"model\":\"cache-fixture\",\"input\":\"question\",\"stream\":true,\"store\":false,"
       "\"prompt_cache_key\":\"owner-scope\",\"prompt_cache_retention\":\"24h\","
       "\"prompt_cache_options\":{\"mode\":\"explicit\",\"ttl\":\"30m\"}}";
   aimee_request_t request = {0};
   char error[128];
   assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_RESPONSES, &request, error,
                                     sizeof error));
   void *previous_context = NULL;
   aimee_ir_session_sink_fn previous = aimee_ir_session_get_sink(&previous_context);
   int failed = aimee_ir_session_capture_failed(), state[2] = {0};
   aimee_ir_session_set_sink(cache_projection_capture, state);
   const aimee_wire_t wires[] = {AIMEE_WIRE_RESPONSES, AIMEE_WIRE_OPENAI_CHAT,
                                 AIMEE_WIRE_ANTHROPIC};
   for (int i = 0; i < 3; i++)
   {
      char *native = agent_render_gateway_request_ir(NULL, &request, wires[i]);
      cJSON *out = native ? cJSON_Parse(native) : NULL;
      assert(out);
      if (i < 2)
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(out, "prompt_cache_key")),
                        "owner-scope"));
      else
         assert(!cJSON_GetObjectItem(out, "prompt_cache_key") && state[0] == 3);
      cJSON_Delete(out);
      free(native);
   }
   state[1] = -1;
   assert(!agent_render_gateway_request_ir(NULL, &request, AIMEE_WIRE_ANTHROPIC));
   assert(state[0] == 4 && aimee_ir_session_capture_failed());
   aimee_ir_session_restore_sink(previous, previous_context, failed);
   aimee_request_free(&request);
}

static int responses_control_projection_capture(const aimee_session_event_t *event, void *context)
{
   int *state = context;
   assert(event->kind == AIMEE_SESSION_PROJECTION && event->projection_request_controls == 4);
   state[0]++;
   return state[1];
}

static void test_actual_responses_control_profile(void)
{
   const char *frame = "{ \"model\":\"boundary-model\",\"input\":\"Keep this task exact.\","
                       "\"stream\":true,\"store\":false,\"max_output_tokens\":123,"
                       "\"temperature\":0.4,\"top_p\":0.8,\"metadata\":{\"source\":\"exact\"},"
                       "\"service_tier\":\"auto\" }\n";
   aimee_request_t source = {0};
   char error[128];
   assert(!aimee_frontend_parse_wire(frame, strlen(frame), AIMEE_WIRE_RESPONSES, &source, error,
                                     sizeof error));
   cJSON *before = aimee_ir_request_json(&source);
   agent_t agent = {0};
   snprintf(agent.provider, sizeof agent.provider, "openai");
   char *wire = agent_render_gateway_request_ir(&agent, &source, AIMEE_WIRE_RESPONSES);
   assert(wire && !strcmp(wire, frame));
   free(wire);
   snprintf(agent.provider, sizeof agent.provider, "chatgpt");
   int state[2] = {0};
   aimee_ir_session_set_sink(responses_control_projection_capture, state);
   wire = agent_render_gateway_request_ir(&agent, &source, AIMEE_WIRE_RESPONSES);
   assert(wire && state[0] == 1 && !strstr(wire, "max_output_tokens") &&
          !strstr(wire, "temperature") && !strstr(wire, "top_p") && !strstr(wire, "metadata") &&
          strstr(wire, "service_tier"));
   free(wire);
   state[1] = 1;
   assert(!agent_render_gateway_request_ir(&agent, &source, AIMEE_WIRE_RESPONSES));
   assert(state[0] == 2 && aimee_ir_session_capture_failed());
   aimee_ir_session_set_sink(NULL, NULL);
   aimee_ir_session_begin_task();
   cJSON *after = aimee_ir_request_json(&source);
   assert(before && after && cJSON_Compare(before, after, 1));
   assert(!strcmp(source.raw_bytes, frame));
   cJSON_Delete(before);
   cJSON_Delete(after);
   aimee_request_free(&source);
}

static void test_cross_wire_control_projection(void)
{
   const char *frame = "{\"model\":\"cross-wire-controls\",\"input\":\"Preserve the "
                       "task.\",\"max_output_tokens\":128,"
                       "\"temperature\":0.3,\"reasoning\":{\"summary\":\"auto\"}}";
   aimee_request_t source = {0};
   char error[128];
   assert(!aimee_frontend_parse_wire(frame, strlen(frame), AIMEE_WIRE_RESPONSES, &source, error,
                                     sizeof error));
   cJSON *before = aimee_ir_request_json(&source);
   const aimee_wire_t targets[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_ANTHROPIC};
   for (int i = 0; i < 2; i++)
   {
      assert(!aimee_backend_render_wire(&source, targets[i]));
      int state[2] = {0};
      aimee_ir_session_set_sink(cache_projection_capture, state);
      char *wire = agent_render_gateway_request_ir(NULL, &source, targets[i]);
      assert(wire && state[0] == 1 && strstr(wire, "Preserve the task.") &&
             strstr(wire, "max_tokens") && !strstr(wire, "summary"));
      free(wire);
      state[1] = 1;
      assert(!agent_render_gateway_request_ir(NULL, &source, targets[i]) && state[0] == 2 &&
             aimee_ir_session_capture_failed());
      aimee_ir_session_set_sink(NULL, NULL);
      aimee_ir_session_begin_task();
   }
   cJSON *after = aimee_ir_request_json(&source);
   assert(before && after && cJSON_Compare(before, after, 1));
   assert(!strcmp(source.raw_bytes, frame));
   cJSON_Delete(before);
   cJSON_Delete(after);
   aimee_request_free(&source);
}

int main(void)
{
   test_cross_wire_control_projection();
   test_actual_responses_control_profile();
   test_actual_ingress_usage_accounting();
   test_cached_unknown_usage();
   test_actual_admission_preparation_mirror();
   test_cache_configuration_at_actual_ingress();
   test_restored_history_rechecks_native_provenance();
   test_responses_run_input_controls();
   test_tool_free_admission_and_text_entry();
   test_owned_chat_admission_uses_canonical_history();
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
         char *native = agent_render_gateway_request_ir(NULL, &canonical, wires[i]);
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
         assert(!agent_render_gateway_request_ir(NULL, &canonical, wires[i]));
         aimee_request_free(&canonical);
      }
   puts("OpenAI ingress canonical admission and independent host-edit policy passed");
   return 0;
}
