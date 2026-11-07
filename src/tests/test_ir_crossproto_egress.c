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

static void test_service_tier_protocol_custody(void)
{
   const aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_RESPONSES};
   const char *inputs[] = {"{\"model\":\"m\",\"service_tier\":\"future-tier\",\"messages\":[{"
                           "\"role\":\"user\",\"content\":\"task\"}]}",
                           "{\"model\":\"m\",\"service_tier\":\"future-tier\",\"input\":\"task\"}"};
   for (int i = 0; i < 2; i++)
   {
      aimee_request_t request = {0};
      char error[128];
      assert(!aimee_frontend_parse_wire(inputs[i], strlen(inputs[i]), wires[i], &request, error,
                                        sizeof error));
      assert(request.service_tier && !strcmp(request.service_tier, "future-tier"));
      for (int j = 0; j < 2; j++)
      {
         char *bytes = aimee_backend_render_wire(&request, wires[j]);
         cJSON *body = bytes ? cJSON_Parse(bytes) : NULL;
         assert(body && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "service_tier")),
                                "future-tier"));
         cJSON_Delete(body);
         free(bytes);
      }
      aimee_request_free(&request);
   }
   const char *invalid[] = {"{\"service_tier\":7,\"messages\":[]}",
                            "{\"service_tier\":7,\"input\":\"task\"}"};
   for (int i = 0; i < 2; i++)
   {
      aimee_request_t request = {0};
      char error[128];
      assert(aimee_frontend_parse_wire(invalid[i], strlen(invalid[i]), wires[i], &request, error,
                                       sizeof error) < 0);
      aimee_request_free(&request);
   }
   puts("  OpenAI Chat/Responses service tier survives canonical cross-protocol custody OK");
}

static void test_responses_control_profiles(void)
{
   aimee_request_t source = {0};
   assert(!aimee_ir_request_from_text(&source, NULL, "Preserve the canonical request.", 123, 0.4));
   source.has_top_p = 1;
   source.top_p = 0.8;
   source.metadata = aimee_ir_json_parse_frame("{\"exact\":9007199254740993123456789}",
                                               strlen("{\"exact\":9007199254740993123456789}"));
   source.service_tier = strdup("auto");
   source.has_parallel_tool_calls = 1;
   source.parallel_tool_calls = 0;
   cJSON *before = aimee_ir_request_json(&source);
   assert(before);
   char *bytes = aimee_backend_render_wire(&source, AIMEE_WIRE_RESPONSES);
   cJSON *body = bytes ? cJSON_Parse(bytes) : NULL;
   assert(body && !aimee_backend_request_control_losses(&source, AIMEE_WIRE_RESPONSES, body));
   assert(cJSON_GetNumberValue(cJSON_GetObjectItem(body, "max_output_tokens")) == 123);
   assert(cJSON_GetNumberValue(cJSON_GetObjectItem(body, "temperature")) == 0.4);
   assert(cJSON_GetNumberValue(cJSON_GetObjectItem(body, "top_p")) == 0.8);
   assert(strstr(bytes, "9007199254740993123456789"));
   assert(cJSON_IsFalse(cJSON_GetObjectItem(body, "parallel_tool_calls")));
   assert(cJSON_IsFalse(cJSON_GetObjectItem(body, "stream")));
   free(bytes);
   cJSON_Delete(body);
   assert(!aimee_backend_render_wire_profile(&source, AIMEE_WIRE_RESPONSES, AIMEE_RESPONSES_CODEX,
                                             NULL));
   int losses = -1;
   bytes = aimee_backend_render_wire_profile(&source, AIMEE_WIRE_RESPONSES, AIMEE_RESPONSES_CODEX,
                                             &losses);
   body = bytes ? cJSON_Parse(bytes) : NULL;
   assert(body && losses == 4 && cJSON_HasObjectItem(body, "service_tier") &&
          cJSON_HasObjectItem(body, "parallel_tool_calls") &&
          !cJSON_HasObjectItem(body, "metadata"));
   free(bytes);
   cJSON_Delete(body);
   source.has_top_k = 1;
   source.top_k = 17;
   assert(!aimee_backend_render_wire(&source, AIMEE_WIRE_RESPONSES));
   source.has_top_k = 0;
   cJSON *after = aimee_ir_request_json(&source);
   assert(after && cJSON_Compare(before, after, 1));
   cJSON_Delete(before);
   cJSON_Delete(after);
   aimee_request_free(&source);
   puts("  Responses API preserves supported controls; Codex requires explicit loss admission OK");
}

static void test_exact_native_request_bytes(void)
{
   const char *bodies[] = {
       "{\n  \"model\": \"native\", \"max_tokens\": 64, \"native_counter\": 9007199254740993, "
       "\"system\": "
       "[{\"type\":\"text\",\"text\":\"prefix\",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":"
       "\"1h\"}}], \"messages\": [{\"role\":\"user\",\"content\":\"hello\"}]\n} \n",
       "{ \"model\": \"native\", \"messages\": [{\"role\":\"user\",\"content\":\"hello\"}] }\n",
       "{ \"model\": \"native\", \"input\": \"hello\", \"store\": true, \"stream\": false }\n"};
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
      if (i == 0)
         assert(strstr(output, "9007199254740993"));
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
   const char *sent = "{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
                      "\"question\"}]}]}";
   aimee_anthropic_replay_t fence;
   assert(!aimee_anthropic_replay_prepare(sent, strlen(sent), &fence));
   assert(!aimee_anthropic_bind_response(&fence, &response));
   view = (aimee_message_t){
       .role = "assistant", .blocks = response.content, .n_blocks = response.n_content};
   persisted = aimee_ir_message_json(&view);
   assert(persisted && aimee_ir_message_parse(persisted, &message) == 0);
   aimee_block_t question = {.type = AIMEE_BLK_TEXT, .text = "question"};
   aimee_message_t replay_messages[] = {{.role = "user", .blocks = &question, .n_blocks = 1},
                                        message};
   request.messages = replay_messages;
   request.n_messages = 2;
   wire = anthropic_backend_build(&request);
   assert(wire);
   parts =
       cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(wire, "messages"), 1), "content");
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
      cJSON *report = NULL;
      cJSON *reported = aimee_frontend_history_import_report(native[wire],
                                                             wire == 0   ? "anthropic"
                                                             : wire == 2 ? "chatgpt"
                                                                         : "openai",
                                                             "owned", "agent", &report);
      assert(reported && report && cJSON_Compare(history, reported, 1));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(report, "outcome")), "normalized"));
      if (wire == 0)
         assert(cJSON_GetObjectItem(report, "removed_thinking_bindings")->valueint > 0);
      cJSON_Delete(reported);
      cJSON_Delete(report);
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
   cJSON *report = NULL;
   const char *canonical =
       "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"original\"}]}]";
   cJSON *exact =
       aimee_frontend_history_import_report(canonical, "anthropic", "owned", "agent", &report);
   assert(exact && report &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(report, "outcome")), "exact"));
   cJSON_Delete(exact);
   cJSON_Delete(report);
   report = NULL;
   assert(!aimee_frontend_history_import_report("screen: tool call may have occurred\n", "tmux-cli",
                                                "owned", "agent", &report));
   assert(report &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(report, "outcome")), "archived"));
   assert(cJSON_GetObjectItem(report, "imported_messages")->valueint == 0);
   cJSON_Delete(report);
   report = NULL;
   assert(!aimee_frontend_history_import_report("", "tmux-cli", "owned", "agent", &report));
   assert(report &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(report, "outcome")), "refused"));
   assert(cJSON_GetObjectItem(report, "archive_chunks")->valueint == 0);
   cJSON_Delete(report);
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

static void test_opaque_json_precision(void)
{
   const char *opaque = "{\"id\":9007199254740993,\"fraction\":0.12345678901234567890123456789,"
                        "\"huge\":1e999,\"tiny\":-1.234e-999,\"zero\":-0.00}";
   cJSON *original = cJSON_ParseWithOpts(opaque, NULL, 1);
   assert(original);
   cJSON *clone = cJSON_Duplicate(original, 1);
   assert(clone);
   cJSON_Delete(original);
   char *printed = cJSON_PrintUnformatted(clone);
   assert(printed && !strcmp(printed, opaque));
   free(printed);
   cJSON *id = cJSON_GetObjectItem(clone, "id");
   cJSON *neighbor = cJSON_ParseWithOpts("9007199254740992", NULL, 1);
   assert(neighbor && !id->valuestring && !neighbor->valuestring &&
          id->valuedouble == neighbor->valuedouble);
   assert(!cJSON_Compare(id, neighbor, 1));
   cJSON *references = cJSON_CreateArray();
   assert(references && cJSON_AddItemReferenceToArray(references, id));
   printed = cJSON_PrintUnformatted(references);
   assert(printed && !strcmp(printed, "[9007199254740993]"));
   free(printed);
   cJSON_SetNumberValue(cJSON_GetArrayItem(references, 0), 7);
   assert(id->number_lexeme && !strcmp(id->number_lexeme, "9007199254740993"));
   cJSON_Delete(references);
   cJSON_SetIntValue(id, 5);
   printed = cJSON_PrintUnformatted(id);
   assert(printed && !strcmp(printed, "5"));
   free(printed);
   id->valuedouble = 6; /* Detect deprecated direct mutation as well. */
   printed = cJSON_PrintUnformatted(id);
   assert(printed && !strcmp(printed, "6"));
   free(printed);
   cJSON_Delete(clone);
   cJSON_Delete(neighbor);
   const char *equal[][2] = {{"1", "1.00"},
                             {"-1200.0", "-12e2"},
                             {"0e9999999999999999999999", "-0"},
                             {"1e999", "10e998"},
                             {"0.0100", "1e-2"}};
   for (size_t i = 0; i < sizeof equal / sizeof equal[0]; i++)
   {
      cJSON *a = cJSON_ParseWithOpts(equal[i][0], NULL, 1);
      cJSON *b = cJSON_ParseWithOpts(equal[i][1], NULL, 1);
      assert(a && b && cJSON_Compare(a, b, 1));
      cJSON_Delete(a);
      cJSON_Delete(b);
   }
   const char *invalid[] = {
       "01", "-01", "1.", "1e", "1e+", "1+2", ".5", "-.5", "{\"id\":\"prefix\\u0000suffix\"}"};
   for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
      assert(!cJSON_ParseWithOpts(invalid[i], NULL, 1));
   puts("  exact opaque decimals, clone/reference lifetime, mutation and equality OK");
}

static void test_opaque_tool_rebuild(void)
{
   const char *body =
       "{\"model\":\"m\",\"max_tokens\":64,\"messages\":[{\"role\":\"assistant\","
       "\"content\":[{\"type\":\"tool_use\",\"id\":\"call\",\"name\":\"f\",\"input\":"
       "{\"id\":9007199254740993,\"fraction\":0.12345678901234567890123456789}}]}],"
       "\"tools\":[{\"name\":\"f\",\"input_schema\":{\"type\":\"object\","
       "\"properties\":{\"id\":{\"const\":9007199254740993}}}}]}";
   aimee_request_t request = {0};
   char error[128];
   assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_ANTHROPIC, &request, error,
                                     sizeof error));
   request.mutated = 1;
   cJSON_Delete(request.raw);
   request.raw = NULL;
   free(request.raw_bytes);
   request.raw_bytes = NULL;
   request.raw_length = 0;
   const aimee_wire_t wires[] = {AIMEE_WIRE_ANTHROPIC, AIMEE_WIRE_OPENAI_CHAT,
                                 AIMEE_WIRE_RESPONSES};
   for (size_t i = 0; i < sizeof wires / sizeof wires[0]; i++)
   {
      char *wire = aimee_backend_render_wire(&request, wires[i]);
      assert(wire && strstr(wire, "9007199254740993") &&
             strstr(wire, "0.12345678901234567890123456789"));
      aimee_request_t restored = {0};
      assert(
          !aimee_frontend_parse_wire(wire, strlen(wire), wires[i], &restored, error, sizeof error));
      char *input = cJSON_PrintUnformatted(restored.messages[0].blocks[0].tool_input);
      assert(input && strstr(input, "9007199254740993") &&
             strstr(input, "0.12345678901234567890123456789"));
      free(input);
      free(wire);
      aimee_request_free(&restored);
   }
   aimee_request_free(&request);
   puts("  opaque tool arguments and schemas survive all three rebuilt native wires OK");
}

static cJSON *argument_field(cJSON *node)
{
   if (cJSON_IsObject(node))
   {
      cJSON *arguments = cJSON_GetObjectItemCaseSensitive(node, "arguments");
      if (arguments)
         return arguments;
   }
   cJSON *child;
   cJSON_ArrayForEach(child, node)
   {
      cJSON *arguments = argument_field(child);
      if (arguments)
         return arguments;
   }
   return NULL;
}
static void test_completed_argument_framing(void)
{
   const char *requests[] = {
       "{\"model\":\"m\",\"messages\":[{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"call\","
       "\"type\":\"function\",\"function\":{\"name\":\"f\",\"arguments\":\"{}\"}}]}]}",
       "{\"model\":\"m\",\"input\":[{\"type\":\"function_call\",\"call_id\":\"call\",\"name\":"
       "\"f\",\"arguments\":\"{}\"}]}"};
   const char *responses[] = {
       "{\"id\":\"r\",\"model\":\"m\",\"choices\":[{\"message\":{\"role\":\"assistant\","
       "\"content\":null,\"tool_calls\":[{\"id\":\"call\",\"type\":\"function\",\"function\":{"
       "\"name\":\"f\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}",
       "{\"id\":\"r\",\"model\":\"m\",\"status\":\"completed\",\"output\":[{\"type\":\"function_"
       "call\",\"id\":\"fc\",\"call_id\":\"call\",\"name\":\"f\",\"arguments\":\"{}\"}]}"};
   const aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_RESPONSES};
   for (int i = 0; i < 2; i++)
      for (int output = 0; output < 2; output++)
      {
         cJSON *frame = cJSON_Parse(output ? responses[i] : requests[i]);
         assert(frame && argument_field(frame));
         for (int invalid = 0; invalid < 2; invalid++)
         {
            assert(cJSON_SetValuestring(argument_field(frame), invalid ? "{} trailing" : "{}"));
            char *wire = cJSON_PrintUnformatted(frame), error[128] = "";
            assert(wire);
            int rc;
            if (output)
            {
               aimee_response_t response = {0};
               rc = aimee_backend_parse_wire(wire, strlen(wire), wires[i], &response, error,
                                             sizeof error);
               aimee_response_free(&response);
            }
            else
            {
               aimee_request_t request = {0};
               rc = aimee_frontend_parse_wire(wire, strlen(wire), wires[i], &request, error,
                                              sizeof error);
               aimee_request_free(&request);
            }
            if (!(invalid ? rc < 0 : rc == 0))
               fprintf(stderr, "argument frame wire=%d output=%d invalid=%d rc=%d error=%s\n", i,
                       output, invalid, rc, error);
            assert(invalid ? rc < 0 : rc == 0);
            free(wire);
         }
         cJSON_Delete(frame);
      }
   puts("  completed request and response arguments reject trailing JSON junk OK");
}

static int thinking_count(const cJSON *native)
{
   int count = 0;
   const cJSON *message, *block;
   cJSON_ArrayForEach(message, cJSON_GetObjectItemCaseSensitive(native, "messages"))
       cJSON_ArrayForEach(block, cJSON_GetObjectItemCaseSensitive(message, "content"))
   {
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "type"));
      if (type && (!strcmp(type, "thinking") || !strcmp(type, "redacted_thinking")))
         count++;
   }
   return count;
}
static void remove_thinking(aimee_request_t *request, int index)
{
   aimee_message_t *message = &request->messages[1];
   aimee_block_free_contents(&message->blocks[index]);
   memmove(&message->blocks[index], &message->blocks[index + 1],
           (size_t)(message->n_blocks - index - 1) * sizeof *message->blocks);
   message->n_blocks--;
   memset(&message->blocks[message->n_blocks], 0, sizeof *message->blocks);
}

static void test_json_comparison_view(void)
{
   cJSON *a = cJSON_Parse("{\"z\":[1.000,9007199254740993],\"a\":{\"b\":10e-1}}");
   cJSON *b = cJSON_Parse("{\"a\":{\"b\":1},\"z\":[1e0,9007199254740993.0]}");
   cJSON *x = aimee_ir_json_canonical_copy(a), *y = aimee_ir_json_canonical_copy(b);
   char *xs = cJSON_PrintUnformatted(x), *ys = cJSON_PrintUnformatted(y);
   assert(xs && ys && !strcmp(xs, ys));
   free(xs);
   free(ys);
   cJSON_Delete(x);
   cJSON_Delete(y);
   assert(!strcmp(a->child->string, "z")); /* source order and decimal tokens stay untouched */
   cJSON_Delete(a);
   cJSON_Delete(b);
   a = cJSON_Parse("{\"input\":{\"a\":1,\"a\":2}}");
   assert(a && !aimee_ir_json_unique(a) && !aimee_ir_json_canonical_copy(a));
   cJSON_Delete(a);
}
static void test_native_thinking_replay_binding(void)
{
   const char *body =
       "{\n\"model\":\"m\",\"max_tokens\":64,\"native_counter\":9007199254740993,"
       "\"system\":[{\"type\":\"text\",\"text\":\"system\",\"cache_control\":{\"type\":"
       "\"ephemeral\",\"ttl\":\"1h\"}}],"
       "\"tools\":[{\"name\":\"f\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"cache_"
       "control\":{\"const\":1}}}}],"
       "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"question\"}]},"
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"one\","
       "\"signature\":\"s1\"},"
       "{\"type\":\"thinking\",\"thinking\":\"two\",\"signature\":\"s2\"},"
       "{\"type\":\"thinking\",\"thinking\":\"three\",\"signature\":\"s3\"},{\"type\":\"text\","
       "\"text\":\"answer\"}]},"
       "{\"role\":\"user\",\"content\":\"continue\"}]}\n";
   for (int scenario = 0; scenario < 8; scenario++)
   {
      aimee_request_t request = {0};
      char error[128];
      assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_ANTHROPIC, &request, error,
                                        sizeof error));
      for (int i = 0; i < 3; i++)
         assert(request.messages[1].blocks[i].thinking_binding &&
                aimee_ir_thinking_binding_valid(request.messages[1].blocks[i].thinking_binding));
      int expected = 3;
      if (scenario == 0)
      {
         request.mutated = 1;
         request.messages[0].blocks[0].context.origin = AIMEE_CTX_ORIGIN_MEMORY;
         request.messages[0].blocks[0].context.model_visible = 1;
         char *exact = aimee_backend_render_wire(&request, AIMEE_WIRE_ANTHROPIC);
         assert(exact && !strcmp(exact, body));
         free(exact);
      }
      if (scenario == 1)
      {
         free(request.system[0].text);
         request.system[0].text = strdup("edited");
         expected = 0;
      }
      if (scenario == 2)
      {
         free(request.system[0].cache_ttl);
         request.system[0].cache_ttl = strdup("5m");
      }
      if (scenario == 3)
      {
         remove_thinking(&request, 0);
         expected = 2;
      }
      if (scenario == 4)
      {
         remove_thinking(&request, 1);
         expected = 1;
      }
      if (scenario == 5)
      {
         cJSON *properties = cJSON_GetObjectItem(request.tools[0].schema, "properties");
         cJSON_SetNumberValue(
             cJSON_GetObjectItem(cJSON_GetObjectItem(properties, "cache_control"), "const"), 2);
         expected = 0; /* A schema field named cache_control is ordinary opaque input. */
      }
      if (scenario == 6)
      {
         free(request.model);
         request.model = strdup("other-model");
         request.max_tokens = 128;
         request.has_max_tokens = 1;
      }
      if (scenario == 7)
      {
         for (int i = 0; i < 3; i++)
         {
            free(request.messages[1].blocks[i].thinking_binding);
            request.messages[1].blocks[i].thinking_binding = NULL;
         }
         expected =
             0; /* Historical signatures without a recorded prefix are not fabricated proof. */
      }
      cJSON *native = anthropic_backend_build(&request);
      assert(native && thinking_count(native) == expected);
      aimee_wire_projection_report_t report;
      aimee_backend_projection_report(&request, AIMEE_WIRE_ANTHROPIC, &report);
      int canonical = scenario == 3 || scenario == 4 ? 2 : 3;
      assert(report.omitted_reasoning_blocks == canonical - expected &&
             report.omitted_opaque_state_blocks == canonical - expected);
      assert(
          request.messages[1].blocks[0].thinking_signature); /* Projection never edits history. */
      cJSON_Delete(native);
      aimee_request_free(&request);
   }
   puts("  native prefix/chain fences, cache-only edits and honest historical omission OK");
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

static int state_guard_calls;
static int strip_unproved_state(aimee_request_t *request, void *context)
{
   state_guard_calls++;
   if (context)
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
static void test_host_state_guard_does_not_mutate_replay_reference(void)
{
   const char *body = "{\"model\":\"m\",\"stream\":true,\"store\":false,\"input\":["
                      "{\"type\":\"reasoning\",\"id\":\"rs_observed\",\"encrypted_content\":"
                      "\"opaque-state\",\"summary\":[]},"
                      "{\"role\":\"user\",\"content\":\"next\"}]}";
   aimee_frontend_state_scope_t previous =
       aimee_frontend_bind_state_guard(strip_unproved_state, NULL);
   aimee_request_t request = {0};
   char error[128];
   assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_RESPONSES, &request, error,
                                     sizeof error));
   assert(state_guard_calls == 1);
   char *wire = aimee_backend_render_wire(&request, AIMEE_WIRE_RESPONSES);
   assert(wire && !strstr(wire, "opaque-state") && !strstr(wire, "rs_observed"));
   assert(state_guard_calls == 1); /* A raw comparison is pure, never guarded again. */
   free(wire);
   aimee_request_free(&request);
   aimee_frontend_state_scope_t outer =
       aimee_frontend_bind_state_guard(strip_unproved_state, (void *)1);
   assert(aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_RESPONSES, &request, error,
                                    sizeof error));
   assert(!request.raw_bytes && !request.messages);
   aimee_frontend_bind_state_guard(outer.guard, outer.context);
   aimee_frontend_bind_state_guard(previous.guard, previous.context);
   assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_RESPONSES, &request, error,
                                     sizeof error));
   wire = aimee_backend_render_wire(&request, AIMEE_WIRE_RESPONSES);
   assert(wire && !strcmp(wire, body));
   free(wire);
   aimee_request_free(&request);
}

static void test_prompt_cache_controls(void)
{
   const aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_RESPONSES};
   for (int source = 0; source < 2; source++)
   {
      const char *content = source ? "\"input\":[{\"role\":\"user\",\"content\":\"question\"}]"
                                   : "\"messages\":[{\"role\":\"user\",\"content\":\"question\"}]";
      char body[1024];
      snprintf(body, sizeof body,
               "{ \"model\":\"cache-fixture\",\"stream\":true,\"store\":false,%s,"
               "\"prompt_cache_key\":\"tenant/user\",\"prompt_cache_retention\":\"24h\","
               "\"prompt_cache_options\":{\"mode\":\"explicit\",\"ttl\":\"30m\","
               "\"opaque\":0.123456789012345678901234567890} }",
               content);
      aimee_request_t request = {0};
      char error[128];
      assert(!aimee_frontend_parse_wire(body, strlen(body), wires[source], &request, error,
                                        sizeof error));
      char *native = aimee_backend_render_wire(&request, wires[source]);
      assert(source ? native == NULL : native && !strcmp(native, body));
      free(native);
      assert(!strcmp(request.prompt_cache_key, "tenant/user") &&
             !strcmp(request.prompt_cache_retention, "24h"));
      for (int target = 0; target < 2; target++)
      {
         int losses = -1;
         native = aimee_backend_render_wire_profile(&request, wires[target], AIMEE_RESPONSES_API,
                                                    &losses);
         assert(losses == (target ? 1 : 0));
         cJSON *output = native ? cJSON_Parse(native) : NULL;
         assert(output &&
                !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(output, "prompt_cache_key")),
                        "tenant/user"));
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(output, "prompt_cache_retention")),
                        "24h"));
         if (!target)
         {
            assert(cJSON_Compare(cJSON_GetObjectItem(output, "prompt_cache_options"),
                                 request.prompt_cache_options, 1));
            assert(strstr(native, "0.123456789012345678901234567890"));
         }
         else
            assert(!cJSON_HasObjectItem(output, "prompt_cache_options"));
         cJSON_Delete(output);
         free(native);
      }
      /* Cache-only mutations must invalidate raw replay even without a dirty flag. */
      for (int change = 0; change < 3; change++)
      {
         aimee_request_free(&request);
         assert(!aimee_frontend_parse_wire(body, strlen(body), wires[source], &request, error,
                                           sizeof error));
         const char *expected;
         if (change == 0)
         {
            free(request.prompt_cache_key);
            request.prompt_cache_key = strdup("new-owner-scope");
            expected = "new-owner-scope";
         }
         else if (change == 1)
         {
            free(request.prompt_cache_retention);
            request.prompt_cache_retention = strdup("in_memory");
            expected = "in_memory";
         }
         else
         {
            cJSON_ReplaceItemInObject(request.prompt_cache_options, "opaque",
                                      cJSON_Parse("0.123456789012345678901234567891"));
            expected = "0.123456789012345678901234567891";
         }
         int losses = -1;
         native = aimee_backend_render_wire_profile(&request, wires[source], AIMEE_RESPONSES_API,
                                                    &losses);
         assert(losses == (source ? 1 : 0));
         assert(native && (source && change == 2 ? !strstr(native, expected)
                                                 : strstr(native, expected) != NULL));
         free(native);
      }
      cJSON *canonical = aimee_ir_request_json(&request);
      assert(canonical && cJSON_Compare(cJSON_GetObjectItem(canonical, "prompt_cache_options"),
                                        request.prompt_cache_options, 1));
      cJSON_Delete(canonical);
      aimee_wire_projection_report_t report;
      aimee_backend_projection_report(&request, AIMEE_WIRE_ANTHROPIC, &report);
      assert(report.omitted_cache_controls == 3);
      aimee_backend_projection_report(&request, wires[source], &report);
      assert(!report.omitted_cache_controls);
      aimee_request_free(&request);
      for (int invalid = 0; invalid < 3; invalid++)
      {
         cJSON *json = cJSON_Parse(body);
         const char *key = (const char *[]){"prompt_cache_key", "prompt_cache_retention",
                                            "prompt_cache_options"}[invalid];
         cJSON_ReplaceItemInObject(json, key, cJSON_CreateArray());
         int rc = source ? responses_frontend_parse(json, &request, error, sizeof error)
                         : openai_frontend_parse(json, &request, error, sizeof error);
         assert(rc && !request.prompt_cache_key && !request.prompt_cache_options);
         aimee_request_free(&request);
         cJSON_Delete(json);
      }
   }
}

typedef struct
{
   aimee_request_t profile;
   int owners, guards, refuse;
} owned_entry_fixture_t;
static int restore_owned_entry(aimee_request_t *request, void *context)
{
   owned_entry_fixture_t *fixture = context;
   fixture->owners++;
   return fixture->refuse ? -1 : aimee_ir_request_clone(&fixture->profile, request);
}
static int inspect_owned_entry(aimee_request_t *request, void *context)
{
   owned_entry_fixture_t *fixture = context;
   fixture->guards++;
   assert(!strcmp(request->model, "host-model"));
   assert(request->n_messages == 1 &&
          !strcmp(request->messages[0].blocks[0].text, "host canonical history"));
   assert(!request->raw_bytes && !request->raw);
   return 0;
}
static void test_host_profile_sidecars_exclude_native_selectors(void)
{
   aimee_request_t profile = {.model = "host-model", .frontend = AIMEE_WIRE_OPENAI_CHAT};
   const char *layouts[] = {
       "{\"model\":\"host-model\",\"chat_template_kwargs\":{\"enable_thinking\":false}}", "{}"};
   const aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_ANTHROPIC};
   for (int i = 0; i < 2; i++)
   {
      profile.frontend = wires[i];
      profile.system_scalar = wires[i] == AIMEE_WIRE_ANTHROPIC;
      profile.raw = cJSON_Parse(layouts[i]);
      assert(aimee_backend_host_profile_valid(&profile));
      if (wires[i] == AIMEE_WIRE_ANTHROPIC)
      {
         cJSON_AddStringToObject(profile.raw, "system", "native system is not a host profile");
         assert(!aimee_backend_host_profile_valid(&profile));
         cJSON_DeleteItemFromObjectCaseSensitive(profile.raw, "system");
      }
      cJSON_AddStringToObject(profile.raw, "conversation", "SDK-side history");
      assert(!aimee_backend_host_profile_valid(&profile));
      cJSON_Delete(profile.raw);
   }
   profile.raw = cJSON_Parse("{\"model\":\"other-model\"}");
   profile.frontend = AIMEE_WIRE_OPENAI_CHAT;
   assert(!aimee_backend_host_profile_valid(&profile));
   profile.frontend = AIMEE_WIRE_RESPONSES;
   assert(!aimee_backend_host_profile_valid(&profile));
   cJSON_Delete(profile.raw);
   profile.raw = NULL;
   assert(aimee_backend_host_profile_valid(&profile));
   profile.frontend = AIMEE_WIRE_UNKNOWN;
   assert(!aimee_backend_host_profile_valid(&profile));
}

static void test_host_request_owner_precedes_proof_guard(void)
{
   aimee_block_t text = {.type = AIMEE_BLK_TEXT, .text = "host canonical history"};
   aimee_message_t message = {.role = "user", .blocks = &text, .n_blocks = 1};
   owned_entry_fixture_t fixture = {.profile = {.model = "host-model",
                                                .messages = &message,
                                                .n_messages = 1,
                                                .has_max_tokens = 1,
                                                .max_tokens = 128,
                                                .mutated = 1}};
   const char *bodies[] = {
       "{\"model\":\"sdk-model\",\"max_tokens\":128,\"system\":\"sdk context\","
       "\"messages\":[{\"role\":\"user\",\"content\":\"sdk history\"}]}",
       "{\"model\":\"sdk-model\",\"messages\":[{\"role\":\"system\",\"content\":\"sdk context\"},"
       "{\"role\":\"user\",\"content\":\"sdk history\"}]}",
       "{\"model\":\"sdk-model\",\"instructions\":\"sdk context\",\"input\":\"sdk history\"}"};
   const aimee_wire_t wires[] = {AIMEE_WIRE_ANTHROPIC, AIMEE_WIRE_OPENAI_CHAT,
                                 AIMEE_WIRE_RESPONSES};
   aimee_frontend_state_scope_t previous_owner =
       aimee_frontend_bind_request_owner(restore_owned_entry, &fixture);
   aimee_frontend_state_scope_t previous_guard =
       aimee_frontend_bind_state_guard(inspect_owned_entry, &fixture);
   for (int i = 0; i < 3; i++)
   {
      aimee_request_t request = {0};
      char error[128];
      int before = fixture.owners;
      int rc = aimee_frontend_parse_wire(bodies[i], strlen(bodies[i]), wires[i], &request, error,
                                         sizeof error);
      assert(!rc && fixture.owners == before + 1);
      request.max_tokens = 17; /* A later approved transform remains effective. */
      rc = aimee_frontend_guard_state(&request, error, sizeof error);
      assert(!rc && request.max_tokens == 17 && fixture.owners == before + 1);
      char *rendered = aimee_backend_render_wire(&request, wires[i]);
      assert(rendered && strstr(rendered, "host canonical history") &&
             !strstr(rendered, "sdk history") && !strstr(rendered, "sdk context"));
      assert(fixture.owners == before + 1); /* Pure comparisons never rebind authority. */
      free(rendered);
      aimee_request_free(&request);
   }
   assert(fixture.owners == 3 && fixture.guards == 6);
   fixture.refuse = 1;
   aimee_request_t refused = {0};
   int rc = aimee_frontend_parse_wire(bodies[2], strlen(bodies[2]), AIMEE_WIRE_RESPONSES, &refused,
                                      NULL, 0);
   assert(rc < 0 && !refused.model && !refused.messages && !refused.raw_bytes &&
          fixture.guards == 6);
   aimee_frontend_bind_request_owner(previous_owner.guard, previous_owner.context);
   aimee_frontend_bind_state_guard(previous_guard.guard, previous_guard.context);
   rc = aimee_frontend_parse_wire(bodies[2], strlen(bodies[2]), AIMEE_WIRE_RESPONSES, &refused,
                                  NULL, 0);
   assert(!rc && !strcmp(refused.model, "sdk-model"));
   aimee_request_free(&refused);
   puts("owned native entry precedes replay proof, preserves later transforms and restores scope");
}

static void test_external_claude_observation_import(void)
{
   const char *source =
       "[{\"type\":\"file-history-snapshot\"},"
       "{\"type\":\"user\",\"sessionId\":\"session\",\"message\":{\"role\":\"user\",\"content\":"
       "\"question\"}},"
       "{\"type\":\"assistant\",\"sessionId\":\"session\",\"message\":{\"role\":\"assistant\","
       "\"content\":["
       "{\"type\":\"thinking\",\"thinking\":\"private reasoning\",\"signature\":\"opaque\"},"
       "{\"type\":\"tool_use\",\"id\":\"external-call\",\"name\":\"Bash\",\"input\":{\"command\":"
       "\"false\"}}]}}]";
   cJSON *records = cJSON_Parse(source);
   cJSON *result = aimee_frontend_claude_transcript_import(records, "session");
   assert(result && cJSON_GetArraySize(result) == 2);
   aimee_message_t message = {0};
   assert(!aimee_ir_message_parse(cJSON_GetArrayItem(result, 1), &message));
   assert(message.n_blocks == 2 && message.blocks[1].type == AIMEE_BLK_TOOL_USE);
   assert(!strcmp(message.blocks[1].tool_id, "external-call"));
   for (int i = 0; i < message.n_blocks; i++)
   {
      assert(message.blocks[i].context.authority == AIMEE_CTX_AUTH_EVIDENCE);
      assert(message.blocks[i].context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
      assert(!strcmp(message.blocks[i].context.revision_domain, "external-sdk"));
      assert(!message.blocks[i].thinking_binding);
   }
   aimee_message_free(&message);
   cJSON_Delete(result);
   assert(!aimee_frontend_claude_transcript_import(records, "foreign"));
   cJSON_Delete(records);
   const char *invalid[] = {
       "[{\"role\":\"user\",\"content\":\"native not envelope\"}]",
       "[{\"type\":\"user\",\"message\":{\"role\":\"assistant\",\"content\":\"mismatch\"}}]",
       "[{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"x\"}},{\"type\":"
       "\"execution\",\"payload\":{\"verification_proofs\":[]}}]",
       "[{\"type\":\"user\",\"isSidechain\":true,\"message\":{\"role\":\"user\",\"content\":\"x\"}}"
       "]",
       "[{\"type\":\"user\",\"type\":\"assistant\",\"message\":{\"role\":\"user\",\"content\":"
       "\"x\"}}]",
       "[{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\","
       "\"text\":\"x\",\"context\":{}}]}}]",
       "[{\"type\":\"summary\",\"summary\":\"unknown reconciliation\"}]"};
   for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
   {
      records = cJSON_Parse(invalid[i]);
      assert(records);
      cJSON *record;
      cJSON_ArrayForEach(record, records)
      {
         const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "type"));
         if (type && (!strcmp(type, "user") || !strcmp(type, "assistant")))
            cJSON_AddStringToObject(record, "sessionId", "session");
      }
      assert(!aimee_frontend_claude_transcript_import(records, "session"));
      cJSON_Delete(records);
   }
   puts("external SDK import: owned typed observations only, no repair/effect/proof authority");
}

static void classified_host_context_keeps_source_history(void)
{
   aimee_block_t system[] = {{.type = AIMEE_BLK_TEXT, .text = "TRUSTED_POLICY"},
                             {.type = AIMEE_BLK_TEXT, .text = "SDK_COMPONENT_IDENTITY"},
                             {.type = AIMEE_BLK_TEXT, .text = "UNTRUSTED_MEMORY"},
                             {.type = AIMEE_BLK_TEXT, .text = "UNTRUSTED_RETRIEVAL"},
                             {.type = AIMEE_BLK_TEXT, .text = "UNTRUSTED_MODEL"},
                             {.type = AIMEE_BLK_TEXT, .text = "UNTRUSTED_TOOL"},
                             {.type = AIMEE_BLK_TEXT, .text = "UNTRUSTED_OPERATOR"},
                             {.type = AIMEE_BLK_TEXT, .text = "UNTRUSTED_PLATFORM"},
                             {.type = AIMEE_BLK_TEXT, .text = "UNTRUSTED_USER"},
                             {.type = AIMEE_BLK_TEXT, .text = "UNTRUSTED_UNKNOWN"},
                             {.type = AIMEE_BLK_TEXT, .text = "HIDDEN_SECRET"}};
   aimee_context_origin_t origins[] = {
       AIMEE_CTX_ORIGIN_PLATFORM,  AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_ORIGIN_MEMORY,
       AIMEE_CTX_ORIGIN_RETRIEVAL, AIMEE_CTX_ORIGIN_MODEL,    AIMEE_CTX_ORIGIN_TOOL,
       AIMEE_CTX_ORIGIN_OPERATOR,  AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_ORIGIN_USER,
       AIMEE_CTX_ORIGIN_UNKNOWN,   AIMEE_CTX_ORIGIN_MEMORY};
   for (int i = 0; i < 11; ++i)
      aimee_ir_block_set_context(
          &system[i], origins[i], i ? AIMEE_CTX_AUTH_EVIDENCE : AIMEE_CTX_AUTH_POLICY,
          AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, i != 10, "source", "scope", 17);
   aimee_ir_block_set_context(&system[1], AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_PUBLIC, 1,
                              "native-sdk-profile", "claude", 0);
   aimee_block_t task = {.type = AIMEE_BLK_TEXT, .text = "ORIGINAL_TASK"};
   aimee_message_t message = {.role = "user", .blocks = &task, .n_blocks = 1, .content_scalar = 1};
   aimee_request_t source = {.model = "fixture",
                             .system = system,
                             .n_system = 11,
                             .messages = &message,
                             .n_messages = 1,
                             .has_max_tokens = 1,
                             .max_tokens = 64};
   assert(aimee_ir_request_needs_model_projection(&source));
   assert(aimee_ir_request_project_model(&source, &source) == -1);
   aimee_request_t view = {0};
   assert(!aimee_ir_request_project_model(&source, &view));
   assert(view.n_system == 2 && view.n_messages == 2 && view.messages[0].n_blocks == 9);
   assert(view.messages[1].content_scalar &&
          !strcmp(view.messages[1].blocks[0].text, "ORIGINAL_TASK"));
   assert(strstr(view.messages[0].blocks[0].text, "not a new task"));
   assert(view.messages[0].blocks[8].context.origin == AIMEE_CTX_ORIGIN_UNKNOWN &&
          view.messages[0].blocks[8].context.authority == AIMEE_CTX_AUTH_EVIDENCE);
   cJSON *first_prefix = aimee_ir_message_json(&view.messages[0]);
   assert(first_prefix);
   assert(view.messages[0].blocks[1].context.revision_epoch == 17);
   assert(!aimee_ir_request_needs_model_projection(&view));
   aimee_request_free(&view);
   const aimee_wire_t wires[] = {AIMEE_WIRE_ANTHROPIC, AIMEE_WIRE_OPENAI_CHAT,
                                 AIMEE_WIRE_RESPONSES};
   for (int i = 0; i < 3; ++i)
   {
      char *body = aimee_backend_render_wire(&source, wires[i]);
      assert(body && strstr(body, "ORIGINAL_TASK") && strstr(body, "UNTRUSTED_MEMORY") &&
             strstr(body, "UNTRUSTED_RETRIEVAL") && strstr(body, "UNTRUSTED_MODEL") &&
             strstr(body, "UNTRUSTED_TOOL") && !strstr(body, "HIDDEN_SECRET"));
      assert(strstr(body, "UNTRUSTED_MEMORY") < strstr(body, "ORIGINAL_TASK"));
      cJSON *native = cJSON_Parse(body);
      assert(native);
      if (wires[i] == AIMEE_WIRE_OPENAI_CHAT)
      {
         const cJSON *row;
         cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(native, "messages"))
         {
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "role"));
            if (role && (!strcmp(role, "system") || !strcmp(role, "developer")))
            {
               char *policy = cJSON_PrintUnformatted(row);
               assert(policy && !strstr(policy, "UNTRUSTED_"));
               free(policy);
            }
         }
      }
      else
      {
         const cJSON *policy = cJSON_GetObjectItemCaseSensitive(
             native, wires[i] == AIMEE_WIRE_ANTHROPIC ? "system" : "instructions");
         char *text = cJSON_PrintUnformatted(policy);
         assert(text && strstr(text, "SDK_COMPONENT_IDENTITY") && !strstr(text, "UNTRUSTED_"));
         free(text);
      }
      cJSON_Delete(native);
      free(body);
      assert(source.n_system == 11 && source.n_messages == 1 && message.n_blocks == 1 &&
             message.content_scalar && !strcmp(system[10].text, "HIDDEN_SECRET"));
   }
   /* Closed host extensions survive filtering; native conversation sidecars do not. */
   source.frontend = AIMEE_WIRE_OPENAI_CHAT;
   source.raw = cJSON_Parse("{\"model\":\"fixture\",\"min_p\":0.07}");
   assert(source.raw);
   char *host_body = aimee_backend_render_wire(&source, AIMEE_WIRE_OPENAI_CHAT);
   cJSON *host_native = host_body ? cJSON_Parse(host_body) : NULL;
   assert(host_native &&
          cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(host_native, "min_p")) == 0.07);
   assert(strstr(host_body, "UNTRUSTED_MEMORY") && !strstr(host_body, "HIDDEN_SECRET"));
   assert(cJSON_GetObjectItemCaseSensitive(source.raw, "min_p") && source.n_messages == 1 &&
          source.n_system == 11 && !strcmp(system[10].text, "HIDDEN_SECRET"));
   cJSON_Delete(host_native);
   free(host_body);
   assert(cJSON_AddArrayToObject(source.raw, "messages"));
   host_body = aimee_backend_render_wire(&source, AIMEE_WIRE_OPENAI_CHAT);
   host_native = host_body ? cJSON_Parse(host_body) : NULL;
   assert(host_native && !cJSON_GetObjectItemCaseSensitive(host_native, "min_p"));
   cJSON_Delete(host_native);
   free(host_body);
   cJSON_Delete(source.raw);
   source.raw = NULL;
   aimee_block_t prior = {.type = AIMEE_BLK_TEXT, .text = "PRIOR_ASSISTANT"};
   aimee_block_t next = {.type = AIMEE_BLK_TEXT, .text = "CURRENT_FOLLOWUP_TASK"};
   aimee_message_t continuation[] = {message,
                                     {.role = "assistant", .blocks = &prior, .n_blocks = 1},
                                     {.role = "user", .blocks = &next, .n_blocks = 1}};
   source.messages = continuation;
   source.n_messages = 3;
   assert(!aimee_ir_request_project_model(&source, &view));
   assert(view.n_messages == 4 && !strcmp(view.messages[0].role, "user") &&
          view.messages[0].n_blocks == 9 &&
          !strcmp(view.messages[3].blocks[0].text, "CURRENT_FOLLOWUP_TASK"));
   cJSON *followup_prefix = aimee_ir_message_json(&view.messages[0]);
   assert(followup_prefix && cJSON_Compare(first_prefix, followup_prefix, 1));
   assert(!strcmp(view.messages[1].blocks[0].text, "ORIGINAL_TASK"));
   assert(source.n_messages == 3 && continuation[0].n_blocks == 1);
   cJSON_Delete(first_prefix);
   cJSON_Delete(followup_prefix);
   aimee_request_free(&view);
   puts("  owned native host evidence projects as user without changing source history OK");
}

static void policy_role_message_evidence_projects_as_user(void)
{
   const char *roles[] = {"system", "developer"};
   const aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_ANTHROPIC,
                                 AIMEE_WIRE_RESPONSES};
   for (int r = 0; r < 2; ++r)
   {
      aimee_block_t blocks[] = {{.type = AIMEE_BLK_TEXT, .text = "POLICY_BEFORE"},
                                {.type = AIMEE_BLK_TEXT, .text = "ROLE_EVIDENCE"},
                                {.type = AIMEE_BLK_TEXT, .text = "POLICY_AFTER"}};
      for (int b = 0; b < 3; ++b)
         aimee_ir_block_set_context(&blocks[b], AIMEE_CTX_ORIGIN_OPERATOR,
                                    b == 1 ? AIMEE_CTX_AUTH_EVIDENCE : AIMEE_CTX_AUTH_POLICY,
                                    AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1,
                                    "source", "role-test", 12);
      aimee_block_t task = {.type = AIMEE_BLK_TEXT, .text = "ROLE_TASK"};
      aimee_message_t messages[] = {{.role = (char *)roles[r], .blocks = blocks, .n_blocks = 3},
                                    {.role = "user", .blocks = &task, .n_blocks = 1}};
      aimee_request_t source = {.model = "fixture",
                                .messages = messages,
                                .n_messages = 2,
                                .has_max_tokens = 1,
                                .max_tokens = 64};
      cJSON *before = aimee_ir_request_json(&source);
      assert(before && aimee_ir_request_needs_model_projection(&source));
      aimee_request_t view = {0};
      assert(!aimee_ir_request_project_model(&source, &view));
      assert(view.n_messages == 3 && !strcmp(view.messages[0].role, "user") &&
             !strcmp(view.messages[0].blocks[1].text, "ROLE_EVIDENCE"));
      assert(!strcmp(view.messages[1].role, roles[r]) && view.messages[1].n_blocks == 2 &&
             !strcmp(view.messages[1].blocks[0].text, "POLICY_BEFORE") &&
             !strcmp(view.messages[1].blocks[1].text, "POLICY_AFTER"));
      assert(!strcmp(view.messages[2].blocks[0].text, "ROLE_TASK"));
      aimee_request_free(&view);
      for (int w = 0; w < 3; ++w)
      {
         char *body = aimee_backend_render_wire(&source, wires[w]);
         cJSON *native = body ? cJSON_Parse(body) : NULL;
         assert(native && strstr(body, "ROLE_EVIDENCE") && strstr(body, "ROLE_TASK"));
         const cJSON *rows = cJSON_GetObjectItemCaseSensitive(
             native, wires[w] == AIMEE_WIRE_RESPONSES ? "input" : "messages");
         const cJSON *row;
         int evidence_user = 0;
         cJSON_ArrayForEach(row, rows)
         {
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "role"));
            char *serialized = cJSON_PrintUnformatted(row);
            assert(serialized);
            if (strstr(serialized, "ROLE_EVIDENCE"))
            {
               assert(role && !strcmp(role, "user"));
               evidence_user++;
            }
            free(serialized);
         }
         assert(evidence_user == 1);
         const cJSON *policy = cJSON_GetObjectItemCaseSensitive(
             native, wires[w] == AIMEE_WIRE_ANTHROPIC ? "system" : "instructions");
         if (policy)
         {
            char *serialized = cJSON_PrintUnformatted(policy);
            assert(serialized && !strstr(serialized, "ROLE_EVIDENCE"));
            free(serialized);
         }
         cJSON_Delete(native);
         free(body);
         cJSON *after = aimee_ir_request_json(&source);
         assert(after && cJSON_Compare(before, after, 1));
         cJSON_Delete(after);
      }
      cJSON_Delete(before);
   }
   puts("  system/developer message evidence projects as user, policy order/source retained OK");
}

static void test_historical_empty_assistant_wire_projection(void)
{
   const char *frame =
       "{\"model\":\"m\",\"max_tokens\":64,\"metadata\":{\"user_id\":\"sdk-correlation\",\"host_"
       "marker\":\"preserved\"},"
       "\"future_provider_setting\":{\"opaque\":9007199254740993},\"messages\":["
       "{\"role\":\"user\",\"content\":\"start\",\"message_extension\":\"preserved\"},"
       "{\"role\":\"assistant\",\"content\":[]},"
       "{\"role\":\"assistant\",\"content\":[]},"
       "{\"role\":\"assistant\",\"content\":[]},"
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"read_"
       "file\",\"input\":{\"path\":\"file\"}}]},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
       "\"content\":[{\"type\":\"text\",\"text\":\"literal result\"}]}]},"
       "{\"role\":\"user\",\"content\":\"continue\"}]}";
   aimee_request_t source = {0};
   char error[128];
   assert(!aimee_frontend_parse_wire(frame, strlen(frame), AIMEE_WIRE_ANTHROPIC, &source, error,
                                     sizeof error));
   cJSON *before = aimee_ir_request_json(&source), *raw = cJSON_Duplicate(source.raw, 1);
   for (int direct = 0; direct < 2; direct++)
   {
      char *wire = NULL;
      cJSON *body = direct ? anthropic_backend_build(&source) : NULL;
      if (!direct)
      {
         wire = aimee_backend_render_wire(&source, AIMEE_WIRE_ANTHROPIC);
         body = wire ? aimee_ir_json_parse_frame(wire, strlen(wire)) : NULL;
      }
      assert(body);
      cJSON *messages = cJSON_GetObjectItemCaseSensitive(body, "messages");
      assert(cJSON_GetArraySize(messages) == 4);
      cJSON *original_messages = cJSON_GetObjectItemCaseSensitive(source.raw, "messages");
      for (int i = 0; i < 4; i++)
         assert(cJSON_Compare(cJSON_GetArrayItem(messages, i),
                              cJSON_GetArrayItem(original_messages, i ? i + 3 : 0), 1));
      assert(cJSON_Compare(cJSON_GetObjectItemCaseSensitive(body, "metadata"),
                           cJSON_GetObjectItemCaseSensitive(source.raw, "metadata"), 1));
      assert(cJSON_Compare(cJSON_GetObjectItemCaseSensitive(body, "future_provider_setting"),
                           cJSON_GetObjectItemCaseSensitive(source.raw, "future_provider_setting"),
                           1));
      char *encoded = cJSON_PrintUnformatted(body);
      assert(encoded && strstr(encoded, "9007199254740993"));
      free(encoded);
      free(wire);
      cJSON_Delete(body);
   }
   char *original_role = source.messages[5].role;
   source.messages[5].role = "tool";
   char *tool_wire = aimee_backend_render_wire(&source, AIMEE_WIRE_ANTHROPIC);
   cJSON *tool_body = tool_wire ? aimee_ir_json_parse_frame(tool_wire, strlen(tool_wire)) : NULL;
   assert(tool_body && !strcmp(source.messages[5].role, "tool"));
   const cJSON *native_result =
       cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(tool_body, "messages"), 2);
   const cJSON *original_result =
       cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(source.raw, "messages"), 5);
   assert(cJSON_Compare(native_result, original_result, 1));
   source.messages[5].role = original_role;
   free(tool_wire);
   cJSON_Delete(tool_body);
   aimee_wire_projection_report_t report;
   aimee_backend_projection_report(&source, AIMEE_WIRE_ANTHROPIC, &report);
   assert(report.omitted_empty_messages == 3 && !report.omitted_reasoning_blocks &&
          !report.omitted_opaque_state_blocks);
   cJSON *after = aimee_ir_request_json(&source);
   assert(after && cJSON_Compare(before, after, 1) && cJSON_Compare(raw, source.raw, 1) &&
          source.raw_length == strlen(frame) && !memcmp(source.raw_bytes, frame, strlen(frame)));
   cJSON_Delete(after);
   cJSON_Delete(before);
   cJSON_Delete(raw);
   aimee_request_free(&source);
   const char *prefill = "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"task\"},{"
                         "\"role\":\"assistant\",\"content\":[]}]}";
   assert(!aimee_frontend_parse_wire(prefill, strlen(prefill), AIMEE_WIRE_ANTHROPIC, &source, error,
                                     sizeof error));
   assert(!anthropic_ir_omitted_empty_messages(&source));
   char *wire = aimee_backend_render_wire(&source, AIMEE_WIRE_ANTHROPIC);
   assert(wire && !strcmp(wire, prefill));
   free(wire);
   aimee_request_free(&source);
   puts("historical empty assistant projection preserves native opaque data and source bytes; "
        "final prefill is unchanged OK");
}

int main(void)
{
   test_historical_empty_assistant_wire_projection();
   policy_role_message_evidence_projects_as_user();
   classified_host_context_keeps_source_history();
   test_external_claude_observation_import();
   test_host_request_owner_precedes_proof_guard();
   test_host_profile_sidecars_exclude_native_selectors();
   test_prompt_cache_controls();
   test_host_state_guard_does_not_mutate_replay_reference();
   test_json_comparison_view();
   test_native_thinking_replay_binding();
   test_completed_argument_framing();
   test_opaque_tool_rebuild();
   test_opaque_json_precision();
   test_mandatory_egress_admission();
   test_owned_tool_catalog();
   test_owned_output_formats();
   test_completed_response_usage_admission();
   test_foreign_reasoning_switch_preserves_canonical_state();
   test_history_entry_migration();
   printf("cross-protocol anthropic egress byte-identity:\n");
   test_service_tier_protocol_custody();
   test_responses_control_profiles();
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
