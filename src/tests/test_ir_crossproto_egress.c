/* Canonical transformed requests use deterministic cross-protocol egress.
 * Untouched native requests separately retain their original cache policy. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <aimee/ir/aimee_ir.h>
#include <aimee/ir/session_event.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"

/* Build the Anthropic egress bytes for a request expressed in `wire`. */
static char *egress(const char *body, int frontend /* 0=anthropic,1=openai */)
{
   cJSON *req = cJSON_Parse(body);
   assert(req);
   aimee_request_t ir;
   char err[128];
   int rc = (frontend == 0) ? anthropic_frontend_parse(req, &ir, err, sizeof err)
                            : openai_frontend_parse(req, &ir, err, sizeof err);
   assert(rc == 0);
   ir.mutated = 1; /* exercise explicit canonical normalization */
   ir.cache_policy = AIMEE_CACHE_STABLE_PREFIX;
   cJSON *out = anthropic_backend_build(&ir);
   assert(out);
   char *s = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   aimee_request_free(&ir);
   cJSON_Delete(req);
   return s;
}

static void same(const char *label, const char *anth_wire, const char *oai_wire)
{
   char *a = egress(anth_wire, 0);
   char *o = egress(oai_wire, 1);
   if (!a || !o || strcmp(a, o) != 0)
   {
      printf("  CROSS-PROTO DRIFT [%s]\n    anthropic-src: %s\n    openai-src   : %s\n", label,
             a ? a : "(null)", o ? o : "(null)");
      free(a);
      free(o);
      exit(1);
   }
   printf("  %s: byte-identical OK\n", label);
   free(a);
   free(o);
}

static void test_exact_native_request_bytes(void)
{
   const char *bodies[] = {
       "{\n  \"model\": \"native\", \"max_tokens\": 64, \"native_counter\": 9007199254740993, "
       "\"system\": "
       "[{\"type\":\"text\",\"text\":\"prefix\",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":"
       "\"1h\"}}], \"messages\": [{\"role\":\"user\",\"content\":\"hello\"}]\n} \n",
       "{ \"model\": \"native\", \"messages\": [{\"role\":\"user\",\"content\":\"hello\"}] }\n",
       "{ \"model\": \"native\", \"input\": \"hello\", \"store\": false, \"stream\": true }\n"};
   const aimee_wire_t wires[] = {AIMEE_WIRE_ANTHROPIC, AIMEE_WIRE_OPENAI_CHAT,
                                 AIMEE_WIRE_RESPONSES};
   for (int i = 0; i < 3; i++)
   {
      aimee_request_t ir = {0};
      char error[128];
      assert(aimee_frontend_parse_wire(bodies[i], strlen(bodies[i]), wires[i], &ir, error,
                                       sizeof error) == 0);
      char *output = aimee_backend_render_wire(&ir, wires[i]);
      assert(output && !strcmp(output, bodies[i]));
      free(output);
      free(ir.model);
      ir.model = strdup("other-model");
      output = aimee_backend_render_wire(&ir, wires[i]);
      cJSON *changed = output ? cJSON_Parse(output) : NULL;
      assert(changed &&
             !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(changed, "model")), "other-model"));
      cJSON_Delete(changed);
      free(output);
      aimee_request_free(&ir);
   }
   aimee_request_t invalid = {0};
   char error[128];
   assert(aimee_frontend_parse_wire("{} trailing", 11, AIMEE_WIRE_ANTHROPIC, &invalid, error,
                                    sizeof error) < 0);
   assert(!invalid.raw && !invalid.raw_bytes);
   puts("  exact native bytes, whitespace, numeric precision and unreported model edits OK");
}
static void test_native_message_group_replay(void)
{
   cJSON *native = cJSON_Parse(
       "{\"status\":\"completed\",\"output\":["
       "{\"type\":\"message\",\"role\":\"assistant\",\"content\":["
       "{\"type\":\"output_text\",\"text\":\"P\"},{\"type\":\"output_text\",\"text\":\"R.\"}]},"
       "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
       "\"text\":\"GitHub\"}]}]}");
   assert(native);
   aimee_response_t response = {0};
   char error[128];
   assert(responses_backend_parse(native, &response, error, sizeof error) == 0);
   assert(response.n_content == 3);
   assert(!strcmp(response.content[2].text, "GitHub"));
   char *display = aimee_ir_response_text_alloc(&response);
   assert(display && !strcmp(display, "PR.\n\nGitHub"));
   free(display);
   cJSON *chat = openai_frontend_render(&response);
   assert(chat);
   const cJSON *first_choice = cJSON_GetArrayItem(cJSON_GetObjectItem(chat, "choices"), 0);
   assert(!strcmp(cJSON_GetStringValue(
                      cJSON_GetObjectItem(cJSON_GetObjectItem(first_choice, "message"), "content")),
                  "PR.\n\nGitHub"));
   cJSON_Delete(chat);
   aimee_message_t view = {
       .role = "assistant", .blocks = response.content, .n_blocks = response.n_content};
   cJSON *saved = aimee_ir_message_json(&view);
   aimee_message_t restored = {0};
   assert(saved && aimee_ir_message_parse(saved, &restored) == 0);
   cJSON_Delete(saved);
   aimee_response_free(&response);
   assert(restored.n_blocks == 3 && restored.blocks[0].message_group == 1 &&
          restored.blocks[1].message_group == 1 && restored.blocks[2].message_group == 2);
   aimee_request_t request = {.model = "m", .messages = &restored, .n_messages = 1};
   cJSON *wire = responses_backend_build(&request);
   assert(wire);
   const cJSON *input = cJSON_GetObjectItem(wire, "input");
   const cJSON *original = cJSON_GetObjectItem(native, "output");
   assert(cJSON_GetArraySize(input) == 2);
   for (int i = 0; i < 2; i++)
      assert(cJSON_Compare(cJSON_GetObjectItem(cJSON_GetArrayItem(input, i), "content"),
                           cJSON_GetObjectItem(cJSON_GetArrayItem(original, i), "content"), 1));
   cJSON_Delete(wire);
   cJSON_Delete(native);
   aimee_message_free(&restored);
   puts("  native message groups replay without injected paragraph bytes OK");
}
static void test_canonical_replay_egress(void)
{
   cJSON *native =
       cJSON_Parse("{\"id\":\"resp_1\",\"status\":\"completed\",\"output\":["
                   "{\"type\":\"reasoning\",\"id\":\"rs_opaque\",\"encrypted_content\":"
                   "\"ciphertext\",\"summary\":[{\"type\":\"summary_text\",\"text\":\"reason\"}]},"
                   "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_"
                   "text\",\"text\":\"before\"}]},"
                   "{\"type\":\"function_call\",\"call_id\":\"opaque-call\",\"name\":\"read_file\","
                   "\"namespace\":\"functions\",\"arguments\":\"{\\\"path\\\":\\\"a.c\\\"}\"},"
                   "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_"
                   "text\",\"text\":\"after\"}]}]}");
   assert(native);
   aimee_response_t response = {0};
   char error[128];
   assert(responses_backend_parse(native, &response, error, sizeof error) == 0);
   cJSON_Delete(native);
   aimee_message_t view = {
       .role = response.role, .blocks = response.content, .n_blocks = response.n_content};
   cJSON *persisted = aimee_ir_message_json(&view);
   assert(persisted);
   aimee_response_free(&response);
   aimee_message_t message = {0};
   assert(aimee_ir_message_parse(persisted, &message) == 0);
   cJSON_Delete(persisted);
   aimee_request_t request = {.model = "m", .messages = &message, .n_messages = 1};
   cJSON *wire = responses_backend_build(&request);
   assert(wire);
   cJSON *items = cJSON_GetObjectItem(wire, "input");
   assert(cJSON_GetArraySize(items) == 4);
   cJSON *after = cJSON_GetArrayItem(items, 3);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(
                      cJSON_GetArrayItem(cJSON_GetObjectItem(after, "content"), 0), "text")),
                  "after"));
   cJSON *reasoning = cJSON_GetArrayItem(items, 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reasoning, "id")), "rs_opaque"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reasoning, "encrypted_content")),
                  "ciphertext"));
   cJSON *call = cJSON_GetArrayItem(items, 2);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(call, "call_id")), "opaque-call"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(call, "namespace")), "functions"));
   cJSON_Delete(wire);
   aimee_message_free(&message);

   /* URL media has no intrinsic MIME type. Inline files retain their data URI. */
   aimee_block_t blocks[] = {
       {.type = AIMEE_BLK_TEXT, .text = "look"},
       {.type = AIMEE_BLK_IMAGE, .media_ref = "https://example.test/image.png"},
       {.type = AIMEE_BLK_DOCUMENT,
        .media_type = "application/pdf",
        .media_ref = "data:application/pdf;base64,AA=="}};
   view = (aimee_message_t){.role = "user", .blocks = blocks, .n_blocks = 3};
   persisted = aimee_ir_message_json(&view);
   assert(persisted && aimee_ir_message_parse(persisted, &message) == 0);
   cJSON_Delete(persisted);
   request.messages = &message;
   wire = openai_backend_build(&request);
   assert(wire);
   cJSON *parts =
       cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(wire, "messages"), 0), "content");
   assert(cJSON_GetArraySize(parts) == 3);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(
                      cJSON_GetObjectItem(cJSON_GetArrayItem(parts, 1), "image_url"), "url")),
                  "https://example.test/image.png"));
   cJSON_Delete(wire);
   wire = responses_backend_build(&request);
   assert(wire && cJSON_GetArraySize(cJSON_GetObjectItem(wire, "input")) == 3);
   cJSON_Delete(wire);
   wire = anthropic_backend_build(&request);
   assert(wire);
   parts =
       cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(wire, "messages"), 0), "content");
   cJSON *source = cJSON_GetObjectItem(cJSON_GetArrayItem(parts, 2), "source");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(source, "data")), "AA=="));
   cJSON_Delete(wire);
   aimee_message_free(&message);

   native = cJSON_Parse("{\"role\":\"assistant\",\"content\":[{\"type\":\"redacted_thinking\","
                        "\"data\":\"opaque-redacted\"}]}");
   assert(anthropic_backend_parse(native, &response, error, sizeof error) == 0);
   assert(response.n_content == 1 && response.content[0].type == AIMEE_BLK_THINKING);
   assert(!strcmp(response.content[0].thinking_redacted, "opaque-redacted"));
   view = (aimee_message_t){
       .role = "assistant", .blocks = response.content, .n_blocks = response.n_content};
   persisted = aimee_ir_message_json(&view);
   assert(persisted && aimee_ir_message_parse(persisted, &message) == 0);
   request.messages = &message;
   wire = anthropic_backend_build(&request);
   assert(wire);
   parts =
       cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(wire, "messages"), 0), "content");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(parts, 0), "data")),
                  "opaque-redacted"));
   cJSON_Delete(wire);
   cJSON_Delete(persisted);
   cJSON_Delete(native);
   aimee_response_free(&response);
   aimee_message_free(&message);
   puts("  canonical persisted reasoning/media replay OK");
}

static void test_history_entry_migration(void)
{
   const char *native[] = {
       "[{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"reason\","
       "\"signature\":\"signed\"},{\"type\":\"tool_use\",\"id\":\"opaque-call\",\"name\":\"read\","
       "\"input\":{}}]},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"opaque-call\","
       "\"content\":\"result\"}]}]",
       "[{\"role\":\"assistant\",\"reasoning_content\":\"reason\",\"content\":null,\"tool_calls\":["
       "{\"id\":\"opaque-call\",\"type\":\"function\",\"function\":{\"name\":\"read\","
       "\"arguments\":\"{}\"}}]},"
       "{\"role\":\"tool\",\"tool_call_id\":\"opaque-call\",\"content\":\"result\"}]",
       "[{\"type\":\"reasoning\",\"id\":\"rs_1\",\"encrypted_content\":\"cipher\",\"summary\":[{"
       "\"type\":\"summary_text\",\"text\":\"reason\"}]},"
       "{\"type\":\"function_call\",\"call_id\":\"opaque-call\",\"name\":\"read\",\"arguments\":\"{"
       "}\"},"
       "{\"type\":\"function_call_output\",\"call_id\":\"opaque-call\",\"output\":\"result\"}]"};
   const aimee_wire_t wires[] = {AIMEE_WIRE_ANTHROPIC, AIMEE_WIRE_OPENAI_CHAT,
                                 AIMEE_WIRE_RESPONSES};
   for (int wire = 0; wire < 3; wire++)
   {
      cJSON *input = cJSON_Parse(native[wire]);
      assert(input);
      cJSON *history = aimee_frontend_history_import(input, wires[wire]);
      assert(history);
      int calls = 0, results = 0, thinking = 0;
      const cJSON *item;
      cJSON_ArrayForEach(item, history)
      {
         assert(!cJSON_GetObjectItemCaseSensitive(item, "content"));
         aimee_message_t message = {0};
         assert(aimee_ir_message_parse(item, &message) == 0);
         for (int j = 0; j < message.n_blocks; j++)
         {
            const aimee_block_t *b = &message.blocks[j];
            if (b->type == AIMEE_BLK_TOOL_USE || b->type == AIMEE_BLK_TOOL_RESULT)
               assert(!strcmp(b->tool_id, "opaque-call"));
            calls += b->type == AIMEE_BLK_TOOL_USE;
            results += b->type == AIMEE_BLK_TOOL_RESULT;
            thinking += b->type == AIMEE_BLK_THINKING;
         }
         aimee_message_free(&message);
      }
      assert(calls == 1 && results == 1 && thinking == 1);
      cJSON *reloaded = aimee_frontend_history_import(history, wires[wire]);
      assert(reloaded && cJSON_Compare(history, reloaded, 1));
      cJSON_Delete(reloaded);
      cJSON *mixed = cJSON_Duplicate(history, 1);
      cJSON_AddItemToArray(mixed, cJSON_Duplicate(cJSON_GetArrayItem(input, 0), 1));
      assert(!aimee_frontend_history_import(mixed, wires[wire]));
      cJSON_Delete(mixed);
      cJSON_Delete(history);
      cJSON_Delete(input);
   }
   printf("  native-history-entry-retains-reasoning-and-tool-pairs OK\n");
}

static void test_native_cache_replay_and_unreported_mutation(void)
{
   cJSON *request =
       cJSON_Parse("{\"model\":\"native\",\"max_tokens\":64,\"native_setting\":{\"keep\":true},"
                   "\"system\":[{\"type\":\"text\",\"text\":\"system\","
                   "\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}}],"
                   "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\","
                   "\"text\":\"hello\",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"5m\"}},"
                   "{\"type\":\"text\",\"text\":\"stable "
                   "tail\",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}}]}],"
                   "\"tools\":[{\"name\":\"keep\",\"input_schema\":{\"type\":\"object\"},\"native_"
                   "tool_option\":true,"
                   "\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}},"
                   "{\"name\":\"remove\",\"input_schema\":{\"type\":\"object\"}}]}");
   aimee_request_t ir = {0};
   char error[128];
   assert(anthropic_frontend_parse(request, &ir, error, sizeof error) == 0);
   cJSON *translated = responses_backend_build(&ir);
   assert(translated);
   const cJSON *function = cJSON_GetArrayItem(cJSON_GetObjectItem(translated, "tools"), 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(function, "type")), "function"));
   assert(cJSON_IsObject(cJSON_GetObjectItem(function, "parameters")) &&
          !cJSON_GetObjectItem(function, "input_schema"));
   cJSON_Delete(translated);

   cJSON *output = anthropic_backend_build(&ir);
   assert(output && cJSON_Compare(request, output, 1));
   char *before = cJSON_PrintUnformatted(request), *after = cJSON_PrintUnformatted(output);
   assert(before && after && !strcmp(before, after));
   free(before);
   free(after);
   cJSON_Delete(output);
   free(ir.messages[0].blocks[0].text);
   ir.messages[0].blocks[0].text = strdup("changed without a mutation flag");
   output = anthropic_backend_build(&ir);
   char *wire = cJSON_PrintUnformatted(output);
   assert(wire && strstr(wire, "changed without a mutation flag"));
   assert(cJSON_IsObject(cJSON_GetObjectItem(output, "native_setting")));
   assert(cJSON_Compare(cJSON_GetObjectItem(output, "system"),
                        cJSON_GetObjectItem(request, "system"), 1));
   assert(cJSON_Compare(cJSON_GetObjectItem(output, "tools"), cJSON_GetObjectItem(request, "tools"),
                        1));
   const cJSON *content = cJSON_GetObjectItem(
       cJSON_GetArrayItem(cJSON_GetObjectItem(output, "messages"), 0), "content");
   const cJSON *original_content = cJSON_GetObjectItem(
       cJSON_GetArrayItem(cJSON_GetObjectItem(request, "messages"), 0), "content");
   assert(
       cJSON_Compare(cJSON_GetArrayItem(content, 1), cJSON_GetArrayItem(original_content, 1), 1));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(
                      cJSON_GetObjectItem(cJSON_GetArrayItem(content, 0), "cache_control"), "ttl")),
                  "5m"));
   free(wire);
   cJSON_Delete(output);
   free(ir.model);
   ir.model = strdup("switched-model");
   output = anthropic_backend_build(&ir);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(output, "model")), "switched-model"));
   cJSON_Delete(output);
   aimee_block_t *grown = realloc(ir.system, (size_t)(ir.n_system + 1) * sizeof *ir.system);
   assert(grown);
   ir.system = grown;
   memmove(&grown[1], &grown[0], (size_t)ir.n_system * sizeof *grown);
   grown[0] = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup("host instructions")};
   ir.n_system++;
   ir.mutated = 1;
   cJSON *patch = cJSON_Parse("[[1]]");
   assert(aimee_ir_remove_tool_paths(&ir, patch) == 1 && ir.mutated);
   cJSON_Delete(patch);
   output = anthropic_backend_build(&ir);
   assert(output && cJSON_GetArraySize(cJSON_GetObjectItem(output, "system")) == 2);
   assert(cJSON_Compare(cJSON_GetArrayItem(cJSON_GetObjectItem(output, "system"), 1),
                        cJSON_GetArrayItem(cJSON_GetObjectItem(request, "system"), 0), 1));
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(output, "tools")) == 1);
   assert(cJSON_Compare(cJSON_GetArrayItem(cJSON_GetObjectItem(output, "tools"), 0),
                        cJSON_GetArrayItem(cJSON_GetObjectItem(request, "tools"), 0), 1));
   cJSON_Delete(output);
   cJSON *saved = aimee_ir_message_json(&ir.messages[0]);
   aimee_message_t restored = {0};
   assert(saved && aimee_ir_message_parse(saved, &restored) == 0);
   assert(!strcmp(restored.blocks[0].cache_ttl, "5m") &&
          !strcmp(restored.blocks[1].cache_ttl, "1h"));
   cJSON_Delete(saved);
   aimee_message_free(&restored);
   aimee_request_free(&ir);
   cJSON_Delete(request);
   puts("  native replay, typed cache lifetimes, insertion and policing preserve client "
        "breakpoints OK");
}

static void test_foreign_reasoning_switch_preserves_canonical_state(void)
{
   cJSON *native =
       cJSON_Parse("{\"model\":\"model-a\",\"input\":["
                   "{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\","
                   "\"text\":\"task\"}]},"
                   "{\"type\":\"reasoning\",\"id\":\"rs-private\",\"encrypted_content\":\"opaque-"
                   "native-state\","
                   "\"summary\":[{\"type\":\"summary_text\",\"text\":\"private reasoning\"}]},"
                   "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_"
                   "text\",\"text\":\"answer\"}]}]}");
   assert(native);
   aimee_request_t request = {0};
   char error[128];
   assert(!responses_frontend_parse(native, &request, error, sizeof error));
   cJSON *saved = cJSON_CreateArray();
   for (int i = 0; i < request.n_messages; i++)
      assert(cJSON_AddItemToArray(saved, aimee_ir_message_json(&request.messages[i])));
   aimee_wire_projection_report_t report;
   aimee_backend_projection_report(&request, AIMEE_WIRE_ANTHROPIC, &report);
   assert(report.omitted_reasoning_blocks == 1 && report.omitted_opaque_state_blocks == 1);
   cJSON *anthropic = anthropic_backend_build(&request);
   assert(anthropic);
   const cJSON *messages = cJSON_GetObjectItemCaseSensitive(anthropic, "messages");
   assert(cJSON_GetArraySize(messages) == 2);
   const cJSON *content =
       cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 1), "content");
   assert(cJSON_GetArraySize(content) == 1);
   assert(!strcmp(cJSON_GetStringValue(
                      cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content, 0), "text")),
                  "answer"));
   cJSON_Delete(anthropic);
   cJSON *after = cJSON_CreateArray();
   for (int i = 0; i < request.n_messages; i++)
      assert(cJSON_AddItemToArray(after, aimee_ir_message_json(&request.messages[i])));
   assert(cJSON_Compare(saved, after, 1));
   cJSON_Delete(after);
   cJSON *replayed = responses_backend_build(&request);
   assert(replayed);
   const cJSON *input = cJSON_GetObjectItemCaseSensitive(replayed, "input");
   int opaque = 0;
   const cJSON *item;
   cJSON_ArrayForEach(item, input)
   {
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "type"));
      if (kind && !strcmp(kind, "reasoning"))
      {
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id")),
                        "rs-private"));
         assert(!strcmp(
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "encrypted_content")),
             "opaque-native-state"));
         opaque++;
      }
   }
   assert(opaque == 1);
   cJSON_Delete(replayed);
   cJSON_Delete(saved);
   aimee_request_free(&request);
   cJSON_Delete(native);
   puts("  model A/B/A native projection retains opaque reasoning in canonical history OK");
}

static void test_completed_response_usage_admission(void)
{
   const char *valid[] = {
       "{\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\","
       "\"text\":\"ok\"}],\"usage\":{"
       "\"input_tokens\":3,\"output_tokens\":7}}",
       "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},\"finish_reason\":"
       "\"stop\"}],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":7,\"completion_tokens_"
       "details\":{\"reasoning_tokens\":2}}}",
       "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
       "\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}],\"usage\":{\"input_tokens\":3,"
       "\"output_tokens\":7,\"output_tokens_details\":{\"reasoning_tokens\":2}}}"};
   aimee_wire_t wires[] = {AIMEE_WIRE_ANTHROPIC, AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_RESPONSES};
   const char *bad[] = {"-1", "1.5", "1e300", "9007199254740992", "\"seven\""};
   for (int i = 0; i < 3; i++)
   {
      aimee_response_t response;
      char error[128];
      assert(!aimee_backend_parse_wire(valid[i], strlen(valid[i]), wires[i], &response, error,
                                       sizeof error));
      assert(response.usage_in == 3 && response.usage_out == 7);
      if (i)
         assert(response.usage_reasoning == 2);
      aimee_response_free(&response);
      cJSON *root = cJSON_Parse(valid[i]);
      cJSON *owner =
          i == 1 ? cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "choices"), 0) : root;
      const char *terminal = i == 0 ? "stop_reason" : i == 1 ? "finish_reason" : "status";
      assert(cJSON_ReplaceItemInObjectCaseSensitive(
          owner, terminal, i == 2 ? cJSON_CreateString("in_progress") : cJSON_CreateNull()));
      char *unfinished = cJSON_PrintUnformatted(root);
      assert(unfinished && aimee_backend_parse_wire(unfinished, strlen(unfinished), wires[i],
                                                    &response, error, sizeof error) < 0);
      free(unfinished);
      cJSON_Delete(root);
      root = cJSON_Parse(valid[i]);
      const char *field = i == 1 ? "completion_tokens" : "output_tokens";
      for (size_t j = 0; j < sizeof bad / sizeof bad[0]; j++)
      {
         cJSON *value = cJSON_Parse(bad[j]);
         assert(value && cJSON_ReplaceItemInObjectCaseSensitive(
                             cJSON_GetObjectItemCaseSensitive(root, "usage"), field, value));
         char *frame = cJSON_PrintUnformatted(root);
         assert(frame && aimee_backend_parse_wire(frame, strlen(frame), wires[i], &response, error,
                                                  sizeof error) < 0);
         free(frame);
      }
      cJSON_Delete(root);
   }
   puts("  completed usage counters reject invalid values before integer conversion OK");
}

static void test_owned_output_formats(void)
{
   const char *strict[] = {"", ",\"strict\":null", ",\"strict\":false", ",\"strict\":true"};
   const aimee_strict_mode_t modes[] = {AIMEE_STRICT_UNSET, AIMEE_STRICT_NULL, AIMEE_STRICT_FALSE,
                                        AIMEE_STRICT_TRUE};
   for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++)
   {
      char body[1024], error[128];
      snprintf(
          body, sizeof body,
          "{\"model\":\"a\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
          "\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"name\":\"result_1\","
          "\"description\":\"exact description\",\"schema\":{\"type\":\"object\"}%s}}}",
          strict[i]);
      cJSON *root = cJSON_Parse(body);
      aimee_request_t ir = {0}, again = {0};
      assert(root && !openai_frontend_parse(root, &ir, error, sizeof error));
      assert(ir.output_format.kind == AIMEE_OUTPUT_JSON_SCHEMA &&
             ir.output_format.strict == modes[i]);
      cJSON_Delete(ir.raw);
      ir.raw = NULL; /* The canonical fields alone must support every boundary. */
      cJSON *responses = responses_backend_build(&ir);
      assert(responses && !responses_frontend_parse(responses, &again, error, sizeof error));
      assert(again.output_format.strict == modes[i]);
      assert(!strcmp(again.output_format.name, "result_1"));
      assert(!strcmp(again.output_format.description, "exact description"));
      assert(cJSON_Compare(ir.output_format.schema, again.output_format.schema, 1));
      cJSON *chat = openai_backend_build(&again);
      assert(chat && cJSON_Compare(cJSON_GetObjectItem(root, "response_format"),
                                   cJSON_GetObjectItem(chat, "response_format"), 1));
      cJSON *native = anthropic_backend_build(&ir);
      assert(native &&
             cJSON_Compare(
                 ir.output_format.schema,
                 cJSON_GetObjectItem(
                     cJSON_GetObjectItem(cJSON_GetObjectItem(native, "output_config"), "format"),
                     "schema"),
                 1));
      cJSON_Delete(native);
      cJSON_Delete(chat);
      cJSON_Delete(responses);
      aimee_request_free(&again);
      aimee_request_free(&ir);
      cJSON_Delete(root);
   }
   cJSON *native =
       cJSON_Parse("{\"model\":\"a\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                   "\"output_config\":{\"effort\":\"high\",\"format\":{\"type\":\"json_schema\","
                   "\"schema\":{\"type\":\"object\"}}}}");
   aimee_request_t ir = {0};
   char error[128];
   assert(!anthropic_frontend_parse(native, &ir, error, sizeof error));
   ir.mutated = 1;
   ir.max_tokens = 128;
   cJSON *out = anthropic_backend_build(&ir);
   assert(out && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(
                             cJSON_GetObjectItem(out, "output_config"), "effort")),
                         "high"));
   assert(cJSON_GetObjectItem(cJSON_GetObjectItem(out, "output_config"), "format"));
   cJSON_Delete(out);
   ir.output_format.kind = AIMEE_OUTPUT_UNSET;
   out = anthropic_backend_build(&ir);
   assert(out && !cJSON_GetObjectItem(cJSON_GetObjectItem(out, "output_config"), "format"));
   cJSON_Delete(out);
   ir.output_format.kind = AIMEE_OUTPUT_JSON_OBJECT;
   assert(!anthropic_backend_build(&ir));
   out = responses_backend_build(&ir);
   assert(out);
   cJSON_Delete(out);
   ir.output_format.kind = AIMEE_OUTPUT_JSON_SCHEMA;
   ir.output_format.strict = (aimee_strict_mode_t)99;
   assert(!openai_family_output_format_build(&ir.output_format, 0));
   aimee_request_free(&ir);
   cJSON_Delete(native);
   puts("  owned structured output preserves schemas and all strict states across model wires OK");
}

static void test_owned_tool_catalog(void)
{
   const char *body =
       "{\"model\":\"native\",\"input\":\"hi\",\"tools\":["
       "{\"type\":\"namespace\",\"name\":\"mcp__aimee\",\"description\":\"catalog\",\"tools\":["
       "{\"type\":\"function\",\"name\":\"Read\",\"parameters\":{\"type\":\"object\"},\"strict\":"
       "true,\"defer_loading\":false}]},"
       "{\"type\":\"web_search\",\"search_context_size\":\"high\",\"filters\":{\"allowed_domains\":"
       "[\"example.test\"]}}]}";
   aimee_request_t request = {0};
   char error[128];
   assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_RESPONSES, &request, error,
                                     sizeof error));
   assert(!aimee_ir_request_admit(&request));
   assert(request.n_tools == 2 && request.tools[0].kind == AIMEE_TOOL_NAMESPACE);
   assert(request.tools[0].n_children == 1 &&
          request.tools[0].children[0].strict == AIMEE_STRICT_TRUE);
   assert(request.tools[0].children[0].has_defer_loading &&
          !request.tools[0].children[0].defer_loading);
   assert(request.tools[1].kind == AIMEE_TOOL_HOSTED && !request.tools[1].name);
   cJSON *original = cJSON_Parse(body);
   cJSON_Delete(request.raw);
   request.raw = NULL;
   for (int i = 0; i < request.n_tools; i++)
   {
      cJSON_Delete(request.tools[i].raw);
      request.tools[i].raw = NULL;
      for (int j = 0; j < request.tools[i].n_children; j++)
      {
         cJSON_Delete(request.tools[i].children[j].raw);
         request.tools[i].children[j].raw = NULL;
      }
   }
   cJSON *native = responses_backend_build(&request);
   assert(native && cJSON_Compare(cJSON_GetObjectItem(original, "tools"),
                                  cJSON_GetObjectItem(native, "tools"), 1));
   assert(!openai_backend_build(&request) && !anthropic_backend_build(&request));
   cJSON_Delete(native);
   free(request.tools[0].children[0].name);
   request.tools[0].children[0].name = strdup("Changed");
   char *changed = aimee_backend_render_wire(&request, AIMEE_WIRE_RESPONSES);
   assert(changed && strstr(changed, "Changed") && !strstr(changed, "\"Read\""));
   free(changed);
   cJSON_Delete(original);
   aimee_request_free(&request);
   const char *anthropic =
       "{\"model\":\"native\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
       "\"tools\":[{\"type\":\"web_search_20250305\",\"name\":\"web_search\",\"max_uses\":3}]}";
   assert(!aimee_frontend_parse_wire(anthropic, strlen(anthropic), AIMEE_WIRE_ANTHROPIC, &request,
                                     error, sizeof error));
   assert(!aimee_ir_request_admit(&request));
   request.cache_policy = AIMEE_CACHE_STABLE_PREFIX;
   cJSON_Delete(request.tools[0].raw);
   request.tools[0].raw = NULL;
   native = anthropic_backend_build(&request);
   const cJSON *tool = cJSON_GetArrayItem(cJSON_GetObjectItem(native, "tools"), 0);
   assert(native &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(tool, "type")), "web_search_20250305"));
   assert(cJSON_GetNumberValue(cJSON_GetObjectItem(tool, "max_uses")) == 3);
   assert(!cJSON_GetObjectItem(tool, "input_schema"));
   cJSON_Delete(native);
   aimee_request_free(&request);
   puts("  typed namespaces, hosted ownership, strict/deferred tools survive without native "
        "sidecars OK");
}

static void test_mandatory_egress_admission(void)
{
   const char *native = "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
   aimee_request_t request = {0};
   char error[128];
   assert(!aimee_frontend_parse_wire(native, strlen(native), AIMEE_WIRE_OPENAI_CHAT, &request,
                                     error, sizeof error));
   /* A pristine native sidecar cannot bypass a malformed owned request. */
   free(request.messages[0].role);
   request.messages[0].role = strdup("forged");
   assert(aimee_ir_request_admit(&request) < 0);
   assert(!anthropic_backend_build(&request));
   assert(!openai_backend_build(&request));
   assert(!responses_backend_build(&request));
   assert(!aimee_backend_render_wire(&request, AIMEE_WIRE_OPENAI_CHAT));
   aimee_request_free(&request);
   puts("  invalid owned requests refuse before native replay or backend construction OK");
}

int main(void)
{
   test_mandatory_egress_admission();
   test_owned_tool_catalog();
   test_owned_output_formats();
   test_completed_response_usage_admission();
   test_foreign_reasoning_switch_preserves_canonical_state();
   test_history_entry_migration();
   printf("cross-protocol anthropic egress byte-identity:\n");
   test_exact_native_request_bytes();
   test_native_message_group_replay();
   test_canonical_replay_egress();
   test_native_cache_replay_and_unreported_mutation();

   /* Text-only turn: same served model, system, and user content in each wire.
    * The anthropic client sends its own cache_control; the openai client sends none.
    * A uniform egress cache policy must normalize both to identical bytes. */
   same("text",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
        "\"system\":[{\"type\":\"text\",\"text\":\"You are helpful.\","
        "\"cache_control\":{\"type\":\"ephemeral\"}}],"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}]}",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
        "\"messages\":[{\"role\":\"system\",\"content\":\"You are helpful.\"},"
        "{\"role\":\"user\",\"content\":\"hi\"}]}");

   /* Tool-bearing multi-turn: system + user + assistant tool_use + user tool_result,
    * plus a tool definition. Exercises tool_input, tool_result shape, and tool schema
    * canonicalization across the frontends. */
   same(
       "tools",
       "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
       "\"system\":[{\"type\":\"text\",\"text\":\"sys\"}],"
       "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"read foo\"}]},"
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":"
       "\"Read\","
       "\"input\":{\"path\":\"foo\"}}]},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
       "\"content\":\"file body\"}]}],"
       "\"tools\":[{\"name\":\"Read\",\"description\":\"Read a file\",\"input_schema\":"
       "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}}]}",
       "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
       "\"messages\":[{\"role\":\"system\",\"content\":\"sys\"},"
       "{\"role\":\"user\",\"content\":\"read foo\"},"
       "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"t1\","
       "\"type\":\"function\",\"function\":{\"name\":\"Read\",\"arguments\":\"{\\\"path\\\":"
       "\\\"foo\\\"}\"}}]},"
       "{\"role\":\"tool\",\"tool_call_id\":\"t1\",\"content\":\"file body\"}],"
       "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"Read\",\"description\":\"Read a "
       "file\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
       "\"string\"}}}}}]}");

   /* Inline base64 image: Anthropic sends a structured base64 source; OpenAI sends a
    * data: URL. The openai frontend must decompose the data: URL to match. */
   same("image-b64",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"source\":"
        "{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"AAAA\"}}]}]}",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":"
        "{\"url\":\"data:image/png;base64,AAAA\"}}]}]}");

   /* tool_result as an Anthropic block-array vs an OpenAI string: the egress collapses
    * a single text block to the string form so both match. */
   same("tool-result-array",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\","
        "\"tool_use_id\":\"t1\",\"content\":[{\"type\":\"text\",\"text\":\"body\"}]}]}]}",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"tool\",\"tool_call_id\":\"t1\",\"content\":\"body\"}]}");

   /* Mixed multi-part user content: text + inline base64 image in one message. */
   same("mixed-content",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"see this\"},"
        "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\","
        "\"data\":\"AAAA\"}}]}]}",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"see this\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,AAAA\"}}]}]}");

   /* Regression: a data: URL where ";base64," appears only AFTER the first comma (i.e.
    * inside the payload, not as the RFC-2397 header marker) must NOT be decomposed --
    * it is kept as a verbatim url reference, not misparsed into a base64 source. */
   {
      char *e = egress("{\"model\":\"m\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\","
                       "\"content\":[{\"type\":\"image_url\",\"image_url\":"
                       "{\"url\":\"data:image/png,payload;base64,tail\"}}]}]}",
                       1);
      assert(e);
      assert(strstr(e, "\"type\":\"url\"") != NULL);                   /* kept as url ref */
      assert(strstr(e, "data:image/png,payload;base64,tail") != NULL); /* verbatim */
      assert(strstr(e, "\"type\":\"base64\"") == NULL);                /* NOT decomposed */
      printf("  data-url-earlier-comma-passthrough OK\n");
      free(e);
   }

   /* Thinking block signature: Anthropic requires the opaque signature echoed back
    * verbatim on a resubmitted assistant thinking turn. With the raw sidecar retired,
    * the canonical rebuild must preserve it (it is modeled, not carried via raw). */
   {
      char *e = egress("{\"model\":\"m\",\"max_tokens\":8,\"messages\":[{\"role\":\"assistant\","
                       "\"content\":[{\"type\":\"thinking\",\"thinking\":\"reasoning\","
                       "\"signature\":\"sig123\"}]}]}",
                       0);
      assert(e);
      assert(strstr(e, "\"signature\":\"sig123\"") != NULL); /* preserved through rebuild */
      printf("  thinking-signature-preserved OK\n");
      free(e);
   }

   /* Redacted thinking is typed and must retain its opaque state at egress. */
   {
      char *e = egress("{\"model\":\"m\",\"max_tokens\":8,\"messages\":[{\"role\":\"assistant\","
                       "\"content\":[{\"type\":\"redacted_thinking\",\"data\":\"Er0Bopaque\"}]}]}",
                       0);
      assert(e);
      assert(strstr(e, "\"redacted_thinking\"") != NULL);
      assert(strstr(e, "Er0Bopaque") != NULL); /* opaque data preserved verbatim */
      printf("  redacted-thinking-preserved OK\n");
      free(e);
   }

   /* Backend cache decoration is transport policy, not economizer policy. Translated
    * requests remain byte-identical across source protocols and retain the stable
    * backend marker regardless of economizer mode. */
   {
      char *a = egress("{\"model\":\"m\",\"max_tokens\":8,"
                       "\"system\":[{\"type\":\"text\",\"text\":\"sys\"}],"
                       "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\","
                       "\"text\":\"hi\"}]}]}",
                       0);
      char *o = egress("{\"model\":\"m\",\"max_tokens\":8,"
                       "\"messages\":[{\"role\":\"system\",\"content\":\"sys\"},"
                       "{\"role\":\"user\",\"content\":\"hi\"}]}",
                       1);
      assert(a && o && strcmp(a, o) == 0);
      assert(strstr(a, "cache_control") != NULL);
      free(a);
      free(o);
      printf("  backend-cache-policy-independent OK\n");
   }

   printf("all cross-protocol egress byte-identity checks passed\n");
   return 0;
}
