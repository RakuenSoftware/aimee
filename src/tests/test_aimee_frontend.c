/* test_aimee_frontend.c -- Slice 1 frontend PARSE: Anthropic + OpenAI requests parse
 * into the IR, and a same-semantic turn in both wires produces IDENTICAL IR
 * (aimee_ir_request_equal) -- the regression proving "no direct translation" +
 * "KB gets the same input regardless of client protocol". The embedded payloads
 * mirror tests/fixtures/ir/{anthropic,openai_chat}_basic_tool.json (the corpus the
 * Slice-3 integration harness loads from disk). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h> /* responses_backend_build: what reaches the provider */
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"

static const char *ANTHROPIC =
    "{\"model\":\"claude-3-5-sonnet-20241022\",\"max_tokens\":1024,"
    "\"system\":[{\"type\":\"text\",\"text\":\"You are a helpful coding assistant.\"}],"
    "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"read foo.c and "
    "summarize it\"}]}],"
    "\"tools\":[{\"name\":\"Read\",\"description\":\"Read a file\",\"input_schema\":"
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}"
    "]}";

static const char *OPENAI =
    "{\"model\":\"gpt-4o\",\"max_tokens\":1024,"
    "\"messages\":[{\"role\":\"system\",\"content\":\"You are a helpful coding assistant.\"},"
    "{\"role\":\"user\",\"content\":\"read foo.c and summarize it\"}],"
    "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"Read\",\"description\":\"Read a "
    "file\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}}]}";

int main(void)
{
   printf("aimee-frontend: ");
   char err[128];

   cJSON *ajson = cJSON_Parse(ANTHROPIC);
   cJSON *ojson = cJSON_Parse(OPENAI);
   assert(ajson && ojson);

   aimee_request_t air, oir;
   assert(anthropic_frontend_parse(ajson, &air, err, sizeof err) == 0);
   assert(openai_frontend_parse(ojson, &oir, err, sizeof err) == 0);

   /* --- structure: Anthropic --- */
   assert(air.frontend == AIMEE_WIRE_ANTHROPIC);
   assert(air.has_max_tokens && air.max_tokens == 1024);
   assert(air.n_system == 1 && air.system[0].type == AIMEE_BLK_TEXT &&
          strcmp(air.system[0].text, "You are a helpful coding assistant.") == 0);
   assert(air.n_messages == 1 && strcmp(air.messages[0].role, "user") == 0);
   assert(air.messages[0].n_blocks == 1 && air.messages[0].blocks[0].type == AIMEE_BLK_TEXT &&
          strcmp(air.messages[0].blocks[0].text, "read foo.c and summarize it") == 0);
   assert(air.n_tools == 1 && strcmp(air.tools[0].name, "Read") == 0 && air.tools[0].schema);

   /* --- structure: OpenAI, with system LIFTED out of messages --- */
   assert(oir.frontend == AIMEE_WIRE_OPENAI_CHAT);
   assert(oir.n_system == 1 && oir.system[0].type == AIMEE_BLK_TEXT); /* lifted */
   assert(oir.n_messages == 1 &&
          strcmp(oir.messages[0].role, "user") == 0); /* system NOT a message */
   assert(oir.n_tools == 1 && strcmp(oir.tools[0].name, "Read") == 0);

   /* --- THE golden cross-protocol assertion: identical IR --- */
   assert(aimee_ir_request_equal(&air, &oir));

   /* --- negative: diverge one byte of content -> not equal --- */
   free(oir.messages[0].blocks[0].text);
   oir.messages[0].blocks[0].text = strdup("read bar.c and summarize it");
   assert(!aimee_ir_request_equal(&air, &oir));

   aimee_request_free(&air);
   aimee_request_free(&oir);
   cJSON_Delete(ajson);
   cJSON_Delete(ojson);

   /* --- tool_use cross-protocol pair: Anthropic input OBJECT == OpenAI arguments
    *     STRING (parsed). Same-semantic assistant turn -> identical IR. --- */
   const char *ATOOL = "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":1024,\"messages\":["
                       "{\"role\":\"assistant\",\"content\":["
                       "{\"type\":\"text\",\"text\":\"I'll read it\"},"
                       "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"Read\",\"input\":{"
                       "\"path\":\"foo.c\"}}]}]}";
   const char *OTOOL = "{\"model\":\"gpt-4o\",\"max_tokens\":1024,\"messages\":["
                       "{\"role\":\"assistant\",\"content\":\"I'll read it\",\"tool_calls\":["
                       "{\"id\":\"toolu_1\",\"type\":\"function\",\"function\":{\"name\":\"Read\","
                       "\"arguments\":\"{\\\"path\\\":\\\"foo.c\\\"}\"}}]}]}";
   cJSON *at = cJSON_Parse(ATOOL), *ot = cJSON_Parse(OTOOL);
   assert(at && ot);
   aimee_request_t atir, otir;
   assert(anthropic_frontend_parse(at, &atir, err, sizeof err) == 0);
   assert(openai_frontend_parse(ot, &otir, err, sizeof err) == 0);
   /* both assistant messages have [TEXT, TOOL_USE] with the same id/name/input */
   assert(atir.messages[0].n_blocks == 2 && otir.messages[0].n_blocks == 2);
   assert(atir.messages[0].blocks[1].type == AIMEE_BLK_TOOL_USE);
   assert(otir.messages[0].blocks[1].type == AIMEE_BLK_TOOL_USE);
   assert(strcmp(otir.messages[0].blocks[1].tool_id, "toolu_1") == 0);
   assert(otir.messages[0].blocks[1].tool_input != NULL); /* parsed from arguments string */
   assert(aimee_ir_request_equal(&atir, &otir)); /* THE tool-args cross-protocol assertion */
   aimee_request_free(&atir);
   aimee_request_free(&otir);
   cJSON_Delete(at);
   cJSON_Delete(ot);

   /* --- render: one IR response -> both client wires (purely from IR) --- */
   aimee_response_t rr;
   memset(&rr, 0, sizeof rr);
   rr.id = strdup("msg_x");
   rr.model = strdup("codex");
   rr.role = strdup("assistant");
   rr.stop_reason = AIMEE_STOP_TOOL_USE;
   rr.n_content = 2;
   rr.content = calloc(2, sizeof(aimee_block_t));
   rr.content[0].type = AIMEE_BLK_TEXT;
   rr.content[0].text = strdup("here you go");
   rr.content[1].type = AIMEE_BLK_TOOL_USE;
   rr.content[1].tool_id = strdup("toolu_9");
   rr.content[1].tool_name = strdup("Read");
   rr.content[1].tool_input = cJSON_Parse("{\"path\":\"foo.c\"}");
   rr.usage_in = 10;
   rr.usage_out = 5;

   /* Anthropic render */
   cJSON *aresp = anthropic_frontend_render(&rr);
   assert(aresp);
   assert(strcmp(cJSON_GetObjectItem(aresp, "type")->valuestring, "message") == 0);
   assert(strcmp(cJSON_GetObjectItem(aresp, "stop_reason")->valuestring, "tool_use") == 0);
   cJSON *ac = cJSON_GetObjectItem(aresp, "content");
   assert(cJSON_GetArraySize(ac) == 2);
   assert(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(ac, 1), "type")->valuestring, "tool_use") ==
          0);
   assert(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(ac, 1), "id")->valuestring, "toolu_9") ==
          0);
   assert(
       (int)cJSON_GetObjectItem(cJSON_GetObjectItem(aresp, "usage"), "input_tokens")->valuedouble ==
       10);

   /* OpenAI render */
   cJSON *oresp = openai_frontend_render(&rr);
   assert(oresp);
   cJSON *ochoice = cJSON_GetArrayItem(cJSON_GetObjectItem(oresp, "choices"), 0);
   assert(strcmp(cJSON_GetObjectItem(ochoice, "finish_reason")->valuestring, "tool_calls") == 0);
   cJSON *omsg = cJSON_GetObjectItem(ochoice, "message");
   assert(strcmp(cJSON_GetObjectItem(omsg, "content")->valuestring, "here you go") == 0);
   cJSON *ocall = cJSON_GetArrayItem(cJSON_GetObjectItem(omsg, "tool_calls"), 0);
   cJSON *ofn = cJSON_GetObjectItem(ocall, "function");
   assert(strcmp(cJSON_GetObjectItem(ofn, "name")->valuestring, "Read") == 0);
   /* arguments re-serialized from the parsed tool_input */
   assert(strstr(cJSON_GetObjectItem(ofn, "arguments")->valuestring, "foo.c"));
   assert(
       (int)cJSON_GetObjectItem(cJSON_GetObjectItem(oresp, "usage"), "total_tokens")->valuedouble ==
       15);

   /* Responses render: output items (message + function_call) */
   cJSON *rresp = responses_frontend_render(&rr);
   assert(rresp);
   assert(strcmp(cJSON_GetObjectItem(rresp, "object")->valuestring, "response") == 0);
   assert(strcmp(cJSON_GetObjectItem(rresp, "status")->valuestring, "completed") == 0);
   cJSON *rout = cJSON_GetObjectItem(rresp, "output");
   assert(cJSON_GetArraySize(rout) == 2);
   int found_fc = 0;
   for (int i = 0; i < cJSON_GetArraySize(rout); i++)
   {
      cJSON *it = cJSON_GetArrayItem(rout, i);
      if (strcmp(cJSON_GetObjectItem(it, "type")->valuestring, "function_call") == 0)
      {
         assert(strcmp(cJSON_GetObjectItem(it, "call_id")->valuestring, "toolu_9") == 0);
         assert(strcmp(cJSON_GetObjectItem(it, "name")->valuestring, "Read") == 0);
         found_fc = 1;
      }
   }
   assert(found_fc);
   cJSON_Delete(rresp);

   cJSON_Delete(aresp);
   cJSON_Delete(oresp);
   aimee_response_free(&rr);

   /* --- THREE-WAY golden: the same semantic turn as Anthropic, OpenAI, AND
    *     Responses parses to IDENTICAL IR. --- */
   cJSON *aj3 = cJSON_Parse(ANTHROPIC);
   cJSON *oj3 = cJSON_Parse(OPENAI);
   const char *RESPONSES =
       "{\"model\":\"gpt-5.5\",\"max_output_tokens\":1024,"
       "\"instructions\":\"You are a helpful coding assistant.\","
       "\"input\":[{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\","
       "\"text\":\"read foo.c and summarize it\"}]}],"
       "\"tools\":[{\"type\":\"function\",\"name\":\"Read\",\"description\":\"Read a file\","
       "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
       "\"required\":[\"path\"]}}]}";
   cJSON *rj3 = cJSON_Parse(RESPONSES);
   assert(aj3 && oj3 && rj3);
   aimee_request_t ai3, oi3, ri3;
   assert(anthropic_frontend_parse(aj3, &ai3, err, sizeof err) == 0);
   assert(openai_frontend_parse(oj3, &oi3, err, sizeof err) == 0);
   assert(responses_frontend_parse(rj3, &ri3, err, sizeof err) == 0);
   assert(ri3.frontend == AIMEE_WIRE_RESPONSES);
   assert(ri3.n_system == 1 && ri3.n_messages == 1 && ri3.n_tools == 1);
   /* all three -> the same IR (max_tokens differs: Responses uses max_output_tokens
    * -> 1024, matching; note anthropic max_tokens 1024 too) */
   assert(aimee_ir_request_equal(&ai3, &oi3));
   assert(aimee_ir_request_equal(&ai3, &ri3)); /* Responses == Anthropic */
   assert(aimee_ir_request_equal(&oi3, &ri3)); /* Responses == OpenAI */
   aimee_request_free(&ai3);
   aimee_request_free(&oi3);
   aimee_request_free(&ri3);
   cJSON_Delete(aj3);
   cJSON_Delete(oj3);
   cJSON_Delete(rj3);

   /* A Codex client sends provider-native tool TYPES alongside its function tools:
    * custom, local_shell, web_search, image_generation. None carries a top-level
    * `name`. Admitting them produced an IR tool with a NULL name, which reached the
    * provider as tools[N].name = "" and made it reject the ENTIRE request
    * (400 empty_string) -- every tool in the catalog lost, and the turn with it.
    *
    * Assert the shape, not a count that will drift: every parsed tool has a usable
    * name, and the named function tool survives. */
   {
      static const char *MIXED =
          "{\"model\":\"m\",\"input\":[{\"type\":\"message\",\"role\":\"user\","
          "\"content\":[{\"type\":\"input_text\",\"text\":\"hi\"}]}],"
          "\"tools\":["
          "{\"type\":\"function\",\"name\":\"Read\",\"description\":\"Read a file\","
          "\"parameters\":{\"type\":\"object\"}},"
          "{\"type\":\"local_shell\"},"
          "{\"type\":\"web_search\"},"
          "{\"type\":\"custom\",\"description\":\"no name here\"},"
          /* The shape a real Codex client sends for an MCP server: a named group
           * whose nested `tools` hold the callable functions. Flattening this lost
           * every tool inside it. */
          "{\"type\":\"namespace\",\"name\":\"mcp__aimee\",\"tools\":["
          "{\"type\":\"function\",\"name\":\"index\",\"parameters\":{\"type\":\"object\"}},"
          "{\"type\":\"function\",\"name\":\"find_symbol\",\"parameters\":{\"type\":\"object\"}}"
          "]}"
          "]}";
      cJSON *mj = cJSON_Parse(MIXED);
      assert(mj);
      aimee_request_t mi;
      assert(responses_frontend_parse(mj, &mi, err, sizeof err) == 0);

      /* Every entry is CARRIED (the sidecar keeps the ones the IR does not model),
       * so nothing a client sent is silently lost. */
      assert(mi.n_tools == 5);
      for (int i = 0; i < mi.n_tools; i++)
         assert(mi.tools[i].raw != NULL);

      /* What actually matters is what reaches the provider. Render and check:
       * the namespace group survives WITH its nested tools, the nameless native
       * types survive as themselves, and no entry is ever emitted with name "" --
       * which is what made the provider reject the whole request. */
      cJSON *rendered = responses_backend_build(&mi);
      assert(rendered);
      cJSON *rt = cJSON_GetObjectItemCaseSensitive(rendered, "tools");
      assert(cJSON_IsArray(rt) && cJSON_GetArraySize(rt) == 5);
      int saw_ns = 0, saw_named_fn = 0, saw_search = 0;
      cJSON *e = NULL;
      cJSON_ArrayForEach(e, rt)
      {
         cJSON *ty = cJSON_GetObjectItemCaseSensitive(e, "type");
         cJSON *nm = cJSON_GetObjectItemCaseSensitive(e, "name");
         if (nm)
            assert(cJSON_IsString(nm) && nm->valuestring[0]); /* never name:"" */
         if (cJSON_IsString(ty) && strcmp(ty->valuestring, "namespace") == 0)
         {
            cJSON *nested = cJSON_GetObjectItemCaseSensitive(e, "tools");
            assert(cJSON_IsArray(nested) && cJSON_GetArraySize(nested) == 2);
            saw_ns = 1;
         }
         if (cJSON_IsString(ty) && strcmp(ty->valuestring, "web_search") == 0)
            saw_search = 1;
         if (cJSON_IsString(ty) && strcmp(ty->valuestring, "function") == 0 && nm &&
             strcmp(nm->valuestring, "Read") == 0)
            saw_named_fn = 1;
      }
      assert(saw_ns && saw_named_fn && saw_search);
      cJSON_Delete(rendered);
      aimee_request_free(&mi);
      cJSON_Delete(mj);
   }

   printf("ok\n");
   return 0;
}
