/* test_aimee_backend.c -- Slice 2: the Anthropic backend build/parse. The headline
 * assertions: (1) same-protocol round-trip through the IR is STABLE
 * (parse->IR->build->parse == equal IR); (2) CROSS-protocol build works — an
 * OpenAI-parsed IR built into an Anthropic request yields the SAME IR an Anthropic
 * client would (proving "no direct translation": OpenAI client -> IR -> Anthropic
 * backend needs zero Anthropic<->OpenAI code). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee_backend.h"
#include "aimee_frontend.h"
#include "aimee_ir.h"
#include "cJSON.h"

static const char *ANTHROPIC =
    "{\"model\":\"claude-3-5-sonnet-20241022\",\"max_tokens\":1024,"
    "\"system\":[{\"type\":\"text\",\"text\":\"You are a helpful coding assistant.\"}],"
    "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"read foo.c\"}]}],"
    "\"tools\":[{\"name\":\"Read\",\"description\":\"Read a file\",\"input_schema\":"
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}"
    "]}";

static const char *OPENAI =
    "{\"model\":\"gpt-4o\",\"max_tokens\":1024,"
    "\"messages\":[{\"role\":\"system\",\"content\":\"You are a helpful coding assistant.\"},"
    "{\"role\":\"user\",\"content\":\"read foo.c\"}],"
    "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"Read\",\"description\":\"Read a "
    "file\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}}]}";

/* parse an Anthropic request JSON into the IR */
static void parse_anthropic(const char *json, aimee_request_t *ir)
{
   char err[128];
   cJSON *j = cJSON_Parse(json);
   assert(j);
   assert(anthropic_frontend_parse(j, ir, err, sizeof err) == 0);
   cJSON_Delete(j);
}

int main(void)
{
   printf("aimee-backend: ");
   char err[128];

   /* --- (1) same-protocol round-trip is IR-stable --- */
   aimee_request_t air;
   parse_anthropic(ANTHROPIC, &air);
   cJSON *built = anthropic_backend_build(&air);
   assert(built);
   aimee_request_t air2;
   assert(anthropic_frontend_parse(built, &air2, err, sizeof err) == 0);
   assert(aimee_ir_request_equal(&air, &air2)); /* parse->build->parse stable */
   /* ...and BYTE-exact: an unmutated same-protocol build ships the raw sidecar, so
    * the wire bytes are identical to the client's (prompt cache preserved). */
   {
      cJSON *orig = cJSON_Parse(ANTHROPIC);
      char *os = cJSON_PrintUnformatted(orig);
      char *bs = cJSON_PrintUnformatted(built);
      assert(os && bs && strcmp(os, bs) == 0);
      free(os);
      free(bs);
      cJSON_Delete(orig);
   }
   cJSON_Delete(built);

   /* --- (1b) ir->mutated forces the typed rebuild; MODELED top-level fields
    *          (top_p, top_k, metadata) survive it -- required so the canonical egress
    *          is byte-faithful once the raw sidecar is retired. --- */
   {
      cJSON *uj = cJSON_Parse("{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,\"messages\":[{"
                              "\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}"
                              "],\"top_p\":0.9,\"top_k\":40,\"metadata\":{\"user_id\":\"u1\"},"
                              "\"service_tier\":\"standard_only\","
                              "\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":1024}}");
      aimee_request_t uir;
      assert(anthropic_frontend_parse(uj, &uir, err, sizeof err) == 0);
      cJSON_Delete(uj);
      cJSON *clean = anthropic_backend_build(&uir); /* mutated=0 -> raw passthrough */
      assert(clean && cJSON_GetObjectItem(clean, "top_p") != NULL); /* preserved */
      uir.mutated = 1;
      cJSON *rebuilt = anthropic_backend_build(&uir); /* typed rebuild */
      cJSON *tp = cJSON_GetObjectItem(rebuilt, "top_p");
      cJSON *tk = cJSON_GetObjectItem(rebuilt, "top_k");
      cJSON *md = cJSON_GetObjectItem(rebuilt, "metadata");
      cJSON *st = cJSON_GetObjectItem(rebuilt, "service_tier");
      cJSON *th = cJSON_GetObjectItem(rebuilt, "thinking");
      assert(tp && tp->valuedouble == 0.9); /* modeled -> survives the rebuild */
      assert(tk && tk->valueint == 40);     /* modeled */
      assert(md && cJSON_IsObject(md));     /* modeled (opaque, preserved) */
      assert(st && cJSON_IsString(st) && strcmp(st->valuestring, "standard_only") == 0);
      assert(th && cJSON_IsObject(th)); /* thinking config (opaque, preserved) */
      cJSON_Delete(clean);
      cJSON_Delete(rebuilt);
      aimee_request_free(&uir);
   }

   /* --- (2) CROSS-protocol BYTE-IDENTITY: the Anthropic egress of the SAME logical
    *         content is byte-identical whether sourced from OpenAI-wire or
    *         Anthropic-wire (Anthropic prompt-caches on exact bytes). The served model
    *         is overridden to the target agent's in the real flow, so align it here;
    *         force air through the typed rebuild (mutated) instead of the raw sidecar. --- */
   cJSON *oj = cJSON_Parse(OPENAI);
   aimee_request_t oir;
   assert(openai_frontend_parse(oj, &oir, err, sizeof err) == 0);
   cJSON_Delete(oj);
   free(oir.model);
   oir.model = strdup("claude-3-5-sonnet");
   free(air.model);
   air.model = strdup("claude-3-5-sonnet");
   air.mutated = 1;
   cJSON *xbuilt = anthropic_backend_build(&oir); /* openai -> IR -> anthropic egress */
   cJSON *abuilt = anthropic_backend_build(&air); /* anthropic -> IR -> anthropic egress */
   assert(xbuilt && abuilt);
   char *xs = cJSON_PrintUnformatted(xbuilt);
   char *as = cJSON_PrintUnformatted(abuilt);
   assert(xs && as && strcmp(xs, as) == 0); /* cross-protocol egress is BYTE-IDENTICAL */
   /* the built Anthropic request has system as a top-level field, not a message */
   assert(cJSON_GetObjectItem(xbuilt, "system") != NULL);
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(xbuilt, "messages")) == 1);
   free(xs);
   free(as);
   cJSON_Delete(xbuilt);
   cJSON_Delete(abuilt);

   aimee_request_free(&air);
   aimee_request_free(&air2);
   aimee_request_free(&oir);

   /* --- (3) response parse: Anthropic response -> IR --- */
   const char *RESP =
       "{\"id\":\"msg_1\",\"model\":\"claude-3-5-sonnet\",\"role\":\"assistant\","
       "\"content\":[{\"type\":\"text\",\"text\":\"hi\"},"
       "{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"Read\",\"input\":{\"path\":\"x\"}}],"
       "\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":12,\"output_tokens\":7}}";
   cJSON *rj = cJSON_Parse(RESP);
   aimee_response_t resp;
   assert(anthropic_backend_parse(rj, &resp, err, sizeof err) == 0);
   assert(strcmp(resp.id, "msg_1") == 0);
   assert(resp.stop_reason == AIMEE_STOP_TOOL_USE);
   assert(resp.n_content == 2 && resp.content[1].type == AIMEE_BLK_TOOL_USE);
   assert(strcmp(resp.content[1].tool_id, "toolu_2") == 0 && resp.content[1].tool_input);
   assert(resp.usage_in == 12 && resp.usage_out == 7);
   aimee_response_free(&resp);
   cJSON_Delete(rj);

   /* --- (4) OpenAI backend: Anthropic client -> IR -> OpenAI request -> re-parse
    *         == equal IR (cross-protocol build via OpenAI). --- */
   aimee_request_t bir;
   parse_anthropic(ANTHROPIC, &bir);
   cJSON *obuilt = openai_backend_build(&bir);
   assert(obuilt);
   aimee_request_t bir2;
   assert(openai_frontend_parse(obuilt, &bir2, err, sizeof err) == 0);
   assert(aimee_ir_request_equal(&bir, &bir2));
   cJSON_Delete(obuilt);
   aimee_request_free(&bir);
   aimee_request_free(&bir2);

   /* OpenAI response -> IR */
   const char *ORESP =
       "{\"id\":\"cmpl_1\",\"model\":\"gpt-4o\",\"choices\":[{\"index\":0,\"message\":"
       "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"call_5\","
       "\"type\":\"function\",\"function\":{\"name\":\"Read\",\"arguments\":\"{\\\"path\\\":"
       "\\\"y\\\"}\"}}]},"
       "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}"
       "}";
   cJSON *orj = cJSON_Parse(ORESP);
   aimee_response_t oresp;
   assert(openai_backend_parse(orj, &oresp, err, sizeof err) == 0);
   assert(oresp.stop_reason == AIMEE_STOP_TOOL_USE);
   assert(oresp.n_content == 1 && oresp.content[0].type == AIMEE_BLK_TOOL_USE);
   assert(strcmp(oresp.content[0].tool_id, "call_5") == 0 && oresp.content[0].tool_input);
   assert(oresp.usage_in == 20 && oresp.usage_out == 8);
   aimee_response_free(&oresp);
   cJSON_Delete(orj);

   /* Reasoning models on the openai wire embed chain-of-thought inline. The IR STORES
    * it as a THINKING block (not discarded) and keeps the answer as TEXT; the content
    * accessor excludes THINKING so callers see only the answer. */
   const char *OTHINK =
       "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\","
       "\"content\":\"<think>hidden reasoning</think>the answer\"}}]}";
   cJSON *otj = cJSON_Parse(OTHINK);
   aimee_response_t othink;
   assert(openai_backend_parse(otj, &othink, err, sizeof err) == 0);
   int n_think = 0, n_text = 0;
   for (int i = 0; i < othink.n_content; i++)
   {
      if (othink.content[i].type == AIMEE_BLK_THINKING)
      {
         n_think++;
         assert(strcmp(othink.content[i].text, "hidden reasoning") == 0); /* STORED */
      }
      if (othink.content[i].type == AIMEE_BLK_TEXT)
      {
         n_text++;
         assert(strcmp(othink.content[i].text, "the answer") == 0);
      }
   }
   assert(n_think == 1 && n_text == 1);
   char buf[64];
   aimee_ir_response_text(&othink, buf, sizeof buf);
   assert(strcmp(buf, "the answer") == 0); /* accessor excludes THINKING */
   aimee_response_free(&othink);
   cJSON_Delete(otj);

   /* Content-parts array (mistral): text parts -> answer, thinking parts -> stored. */
   const char *OARR =
       "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\","
       "\"content\":[{\"type\":\"thinking\",\"thinking\":[{\"type\":\"text\",\"text\":\"why\"}]},"
       "{\"type\":\"text\",\"text\":\"final\"}]}}]}";
   cJSON *oaj = cJSON_Parse(OARR);
   aimee_response_t oarr;
   assert(openai_backend_parse(oaj, &oarr, err, sizeof err) == 0);
   aimee_ir_response_text(&oarr, buf, sizeof buf);
   assert(strcmp(buf, "final") == 0);
   int arr_think = 0;
   for (int i = 0; i < oarr.n_content; i++)
      if (oarr.content[i].type == AIMEE_BLK_THINKING)
         arr_think++;
   assert(arr_think == 1); /* thinking part stored, not dropped */
   aimee_response_free(&oarr);
   cJSON_Delete(oaj);

   /* --- (5) Responses (codex) backend: Anthropic IR -> Responses request; a
    *         Responses response (message + function_call output items) -> IR. --- */
   aimee_request_t cir;
   parse_anthropic(ANTHROPIC, &cir);
   cJSON *cbuilt = responses_backend_build(&cir);
   assert(cbuilt);
   assert(cJSON_GetObjectItem(cbuilt, "instructions")); /* system -> instructions */
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(cbuilt, "input")) >= 1); /* messages -> items */
   assert(cJSON_GetObjectItem(cbuilt, "tools"));
   cJSON_Delete(cbuilt);
   aimee_request_free(&cir);

   const char *CRESP = "{\"id\":\"resp_1\",\"status\":\"completed\",\"output\":["
                       "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":"
                       "\"output_text\",\"text\":\"done\"}]},"
                       "{\"type\":\"function_call\",\"call_id\":\"fc_1\",\"name\":\"Read\","
                       "\"arguments\":\"{\\\"path\\\":\\\"z\\\"}\"}],"
                       "\"usage\":{\"input_tokens\":30,\"output_tokens\":9}}";
   cJSON *crj = cJSON_Parse(CRESP);
   aimee_response_t cresp;
   assert(responses_backend_parse(crj, &cresp, err, sizeof err) == 0);
   assert(cresp.stop_reason == AIMEE_STOP_TOOL_USE); /* a function_call was emitted */
   assert(cresp.n_content == 2);
   assert(cresp.content[0].type == AIMEE_BLK_TEXT && strcmp(cresp.content[0].text, "done") == 0);
   assert(cresp.content[1].type == AIMEE_BLK_TOOL_USE &&
          strcmp(cresp.content[1].tool_id, "fc_1") == 0 && cresp.content[1].tool_input);
   assert(cresp.usage_in == 30 && cresp.usage_out == 9);
   aimee_response_free(&cresp);
   cJSON_Delete(crj);

   /* --- (6) tool_result SPLIT (grouping ruling, Option A): an Anthropic tool
    *         conversation -> IR -> OpenAI emits a role:tool message + Responses a
    *         function_call_output, both carrying the tool_id VERBATIM. --- */
   const char *CONVO =
       "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":1024,\"messages\":["
       "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"read foo.c\"}]},"
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":"
       "\"Read\",\"input\":{\"path\":\"foo.c\"}}]},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\","
       "\"content\":\"file contents\"}]}]}";
   aimee_request_t conv;
   parse_anthropic(CONVO, &conv);
   /* IR: the 3rd message is a user message with a single TOOL_RESULT block */
   assert(conv.n_messages == 3);
   assert(conv.messages[2].n_blocks == 1 &&
          conv.messages[2].blocks[0].type == AIMEE_BLK_TOOL_RESULT &&
          strcmp(conv.messages[2].blocks[0].tool_id, "toolu_1") == 0);

   /* OpenAI backend: a role:"tool" message with tool_call_id=toolu_1 */
   cJSON *ob = openai_backend_build(&conv);
   cJSON *om = cJSON_GetObjectItem(ob, "messages");
   int found_tool = 0;
   for (int k = 0; k < cJSON_GetArraySize(om); k++)
   {
      cJSON *mm = cJSON_GetArrayItem(om, k);
      cJSON *role = cJSON_GetObjectItem(mm, "role");
      if (role && strcmp(role->valuestring, "tool") == 0)
      {
         assert(strcmp(cJSON_GetObjectItem(mm, "tool_call_id")->valuestring, "toolu_1") == 0);
         assert(strcmp(cJSON_GetObjectItem(mm, "content")->valuestring, "file contents") == 0);
         found_tool = 1;
      }
   }
   assert(found_tool);
   /* full round-trip: Anthropic tool convo -> IR -> OpenAI wire (SPLIT) -> IR
    * (MERGE) == the original IR. The split (backend) and merge (frontend) are
    * inverses over the common subset. */
   aimee_request_t conv_rt;
   assert(openai_frontend_parse(ob, &conv_rt, err, sizeof err) == 0);
   assert(aimee_ir_request_equal(&conv, &conv_rt));
   aimee_request_free(&conv_rt);
   cJSON_Delete(ob);

   /* Responses backend: a function_call_output item with call_id=toolu_1 */
   cJSON *cb = responses_backend_build(&conv);
   cJSON *ci = cJSON_GetObjectItem(cb, "input");
   int found_fco = 0;
   for (int k = 0; k < cJSON_GetArraySize(ci); k++)
   {
      cJSON *it = cJSON_GetArrayItem(ci, k);
      cJSON *ty = cJSON_GetObjectItem(it, "type");
      if (ty && strcmp(ty->valuestring, "function_call_output") == 0)
      {
         assert(strcmp(cJSON_GetObjectItem(it, "call_id")->valuestring, "toolu_1") == 0);
         found_fco = 1;
      }
   }
   assert(found_fco);
   cJSON_Delete(cb);
   aimee_request_free(&conv);

   printf("ok\n");
   return 0;
}
