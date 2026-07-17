/* test_aimee_ir_serve.c -- Slice 5 core: build a provider request from an inbound
 * Anthropic request VIA THE IR (no direct translation), for the Responses (codex)
 * and OpenAI backends, with the served model overridden to the agent's. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "agent_protocol.h"
#include "aimee_ir.h"
#include "aimee_ir_serve.h"
#include "cJSON.h"

static const char *REQ =
    "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
    "\"system\":[{\"type\":\"text\",\"text\":\"be helpful\"}],"
    "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}],"
    "\"tools\":[{\"name\":\"Read\",\"input_schema\":{\"type\":\"object\"}}]}";

int main(void)
{
   printf("ir-serve: ");
   cJSON *req = cJSON_Parse(REQ);
   assert(req);

   /* Responses (codex) backend: model overridden, max_tokens override applied */
   char *rbody = aimee_ir_build_provider_body(req, "chatgpt", "gpt-5.5-codex", 200, 1);
   assert(rbody);
   cJSON *rj = cJSON_Parse(rbody);
   assert(rj);
   assert(strcmp(cJSON_GetObjectItem(rj, "model")->valuestring, "gpt-5.5-codex") ==
          0); /* agent's */
   /* codex requirements (verified live): store=false, stream=true, no max_output_tokens */
   assert(cJSON_IsFalse(cJSON_GetObjectItem(rj, "store")));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(rj, "stream")));
   assert(cJSON_GetObjectItem(rj, "max_output_tokens") == NULL);
   assert(cJSON_GetObjectItem(rj, "instructions")); /* system -> instructions */
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(rj, "input")) >= 1);
   assert(cJSON_GetObjectItem(rj, "tools"));
   cJSON_Delete(rj);
   free(rbody);

   /* OpenAI backend: model overridden, no max_tokens override -> IR's 100 kept */
   char *obody = aimee_ir_build_provider_body(req, "openai", "some-openai-model", 0, 1);
   assert(obody);
   cJSON *oj = cJSON_Parse(obody);
   assert(oj);
   assert(strcmp(cJSON_GetObjectItem(oj, "model")->valuestring, "some-openai-model") == 0);
   assert((int)cJSON_GetObjectItem(oj, "max_tokens")->valuedouble == 100); /* from IR */
   /* messages: a leading system message + the user message */
   cJSON *msgs = cJSON_GetObjectItem(oj, "messages");
   assert(cJSON_GetArraySize(msgs) == 2);
   assert(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 0), "role")->valuestring, "system") ==
          0);
   assert(cJSON_GetObjectItem(oj, "tools"));
   cJSON_Delete(oj);
   free(obody);

   /* want_stream is the CALLER's decision, not the client's. The request fixture has
    * stream:true, but a buffered-replay caller must be able to ask the upstream for a
    * whole JSON reply — inheriting the client's flag is what made that path request
    * SSE and then parse it as JSON ("unparseable reply"). */
   {
      char *nb = aimee_ir_build_provider_body(req, "openai", "m", 0, 0);
      assert(nb);
      cJSON *nj = cJSON_Parse(nb);
      assert(nj);
      assert(cJSON_GetObjectItem(nj, "stream") == NULL); /* not merely false: absent */
      cJSON_Delete(nj);
      free(nb);

      char *sb = aimee_ir_build_provider_body(req, "openai", "m", 0, 1);
      assert(sb);
      cJSON *sj = cJSON_Parse(sb);
      assert(sj);
      assert(cJSON_IsTrue(cJSON_GetObjectItem(sj, "stream")));
      cJSON_Delete(sj);
      free(sb);
   }

   /* bad request -> NULL (caller falls back to legacy) */
   assert(aimee_ir_build_provider_body(NULL, "openai", "m", 0, 1) == NULL);
   cJSON_Delete(req);

   /* aimee_ir_responses_to_chat: a Responses body -> chat components via the IR
    * (system lifted to instructions, input -> chat messages) */
   const char *RBODY = "{\"model\":\"gpt-5.5\",\"stream\":true,\"instructions\":\"be helpful\","
                       "\"input\":[{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":"
                       "\"input_text\",\"text\":\"hi\"}]}],"
                       "\"tools\":[{\"type\":\"function\",\"name\":\"Read\",\"parameters\":{"
                       "\"type\":\"object\"}}]}";
   char mdl[64];
   char *instr = NULL;
   cJSON *rmsgs = NULL, *rtls = NULL;
   int strm = 0;
   assert(aimee_ir_responses_to_chat(RBODY, mdl, sizeof mdl, &instr, &rmsgs, &rtls, &strm) == 0);
   assert(strcmp(mdl, "gpt-5.5") == 0);
   assert(strm == 1);
   assert(instr && strcmp(instr, "be helpful") == 0);
   assert(rmsgs && cJSON_GetArraySize(rmsgs) == 1);
   assert(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(rmsgs, 0), "role")->valuestring, "user") ==
          0);
   assert(rtls && cJSON_GetArraySize(rtls) == 1);
   free(instr);
   cJSON_Delete(rmsgs);
   cJSON_Delete(rtls);

   /* aimee_ir_build_from_chat: agent-path chat components -> provider request via IR */
   cJSON *cm = cJSON_Parse("[{\"role\":\"user\",\"content\":\"hi\"}]");
   cJSON *ct = cJSON_Parse("[{\"type\":\"function\",\"function\":{\"name\":\"Read\",\"parameters\":"
                           "{\"type\":\"object\"}}}]");
   cJSON *fc = aimee_ir_build_from_chat("gpt-5.5-codex", cm, ct, "be helpful", "chatgpt");
   assert(fc);
   assert(strcmp(cJSON_GetObjectItem(fc, "model")->valuestring, "gpt-5.5-codex") == 0);
   assert(cJSON_IsFalse(cJSON_GetObjectItem(fc, "store"))); /* codex req shape */
   assert(cJSON_GetObjectItem(fc, "instructions"));         /* system -> instructions */
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(fc, "input")) >= 1);
   cJSON_Delete(fc);
   cJSON_Delete(cm);
   cJSON_Delete(ct);

   /* Slice 3: aimee_ir_response_to_parsed adapter (IR response -> parsed_response_t).
    * Exhaustive per the roundtable ruling: text, tool calls, tokens, cache tokens,
    * stop_reason, model, and the NULL guard. */
   {
      aimee_block_t b1 = {0};
      b1.type = AIMEE_BLK_TEXT;
      b1.text = (char *)"hello world";
      aimee_response_t r1 = {0};
      r1.model = (char *)"minimax-m3";
      r1.raw_stop_reason = (char *)"stop";
      r1.content = &b1;
      r1.n_content = 1;
      r1.usage_in = 12;
      r1.usage_out = 5;
      r1.usage_cache_read = 3;
      r1.usage_cache_write = 7;
      parsed_response_t p1;
      aimee_ir_response_to_parsed(&r1, &p1);
      assert(p1.content && strcmp(p1.content, "hello world") == 0);
      assert(p1.is_tool_call == 0 && p1.call_count == 0);
      assert(p1.prompt_tokens == 12 && p1.completion_tokens == 5);
      assert(p1.cache_read_tokens == 3 && p1.cache_write_tokens == 7);
      assert(strcmp(p1.model, "minimax-m3") == 0);
      assert(strcmp(p1.stop_reason, "stop") == 0);
      free(p1.content);

      cJSON *ti = cJSON_CreateObject();
      cJSON_AddStringToObject(ti, "city", "Paris");
      aimee_block_t b2 = {0};
      b2.type = AIMEE_BLK_TOOL_USE;
      b2.tool_id = (char *)"call_abc";
      b2.tool_name = (char *)"get_weather";
      b2.tool_input = ti;
      aimee_response_t r2 = {0};
      r2.raw_stop_reason = (char *)"tool_use";
      r2.content = &b2;
      r2.n_content = 1;
      parsed_response_t p2;
      aimee_ir_response_to_parsed(&r2, &p2);
      assert(p2.is_tool_call == 1 && p2.call_count == 1);
      assert(strcmp(p2.calls[0].id, "call_abc") == 0);
      assert(strcmp(p2.calls[0].name, "get_weather") == 0);
      assert(p2.calls[0].arguments && strstr(p2.calls[0].arguments, "Paris"));
      assert(p2.content == NULL); /* pure tool call -> no text */
      free(p2.calls[0].arguments);
      cJSON_Delete(ti);

      parsed_response_t p3;
      aimee_ir_response_to_parsed(NULL, &p3);
      assert(p3.call_count == 0 && p3.content == NULL && p3.is_tool_call == 0);
   }

   printf("ok\n");
   return 0;
}
