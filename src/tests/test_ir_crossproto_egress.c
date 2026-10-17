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
   ir.mutated = 1; /* exercise canonical rebuild, not native replay */
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
   assert(cJSON_GetArraySize(items) == 5);
   cJSON *separator = cJSON_GetArrayItem(items, 3);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(
                      cJSON_GetArrayItem(cJSON_GetObjectItem(separator, "content"), 0), "text")),
                  "\n\n"));
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
                   "\"text\":\"hello\",\"cache_control\":{\"type\":\"ephemeral\"}}]}]}");
   aimee_request_t ir = {0};
   char error[128];
   assert(anthropic_frontend_parse(request, &ir, error, sizeof error) == 0);
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
   free(wire);
   cJSON_Delete(output);
   free(ir.model);
   ir.model = strdup("switched-model");
   output = anthropic_backend_build(&ir);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(output, "model")), "switched-model"));
   cJSON_Delete(output);
   aimee_request_free(&ir);
   cJSON_Delete(request);
   puts("  native cache replay and unreported mutation detection OK");
}

int main(void)
{
   test_history_entry_migration();
   printf("cross-protocol anthropic egress byte-identity:\n");
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
