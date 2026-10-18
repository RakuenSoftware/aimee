/* test_aimee_ir_serve.c -- Slice 5 core: build a provider request from an inbound
 * Anthropic request VIA THE IR (no direct translation), for the Responses (codex)
 * and OpenAI backends, with the served model overridden to the agent's. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "agent_protocol.h"
#include <aimee/ir/aimee_ir.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "aimee_ir_serve.h"
#include "cJSON.h"

/* The memory module + config toggles that aimee_ir_serve.c now references (memory is
 * registered on the IR transform seam) are satisfied by tests/support/
 * ir_seam_memory_stub.o -- stubbed DISABLED, so this build/translation-parity suite
 * stays a minimal link and its byte-exact assertions are unchanged. */

static const char *REQ =
    "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
    "\"system\":[{\"type\":\"text\",\"text\":\"be helpful\"}],"
    "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}],"
    "\"tools\":[{\"name\":\"Read\",\"input_schema\":{\"type\":\"object\"}}]}";

void ir_seam_test_session(const char *session_id);
void ir_seam_test_persona(const char *instructions);
void ir_seam_test_memory(int enabled);
void ir_seam_test_native_preinject(int enabled);
int ir_seam_test_plan_calls(const char *phase);
const char *ir_seam_test_query(void);
int ir_seam_test_guidance_provided(void);

/* A provider change imports legacy history once, then reloads canonical IR. */
static void test_import_history(void)
{
   static const char *ANTH =
       "[{\"role\":\"user\",\"content\":\"read a.c\"},"
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"reading\"},"
       "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"Read\",\"input\":{\"path\":\"a.c\"}}]}"
       ","
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\","
       "\"content\":\"int main;\"}]},"
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"done\"}]}]";

   cJSON *native = cJSON_Parse(ANTH);
   cJSON *history = aimee_frontend_history_import(native, AIMEE_WIRE_ANTHROPIC);
   assert(history && cJSON_GetArraySize(history) == 4);
   cJSON *call =
       cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(history, 1), "blocks"), 1);
   cJSON *result =
       cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(history, 2), "blocks"), 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(call, "tool_id")),
                  cJSON_GetStringValue(cJSON_GetObjectItem(result, "tool_id"))));
   cJSON *reload = aimee_frontend_history_import(history, AIMEE_WIRE_RESPONSES);
   assert(reload && cJSON_Compare(history, reload, 1));
   cJSON_Delete(reload);
   cJSON_Delete(history);
   cJSON_Delete(native);
}

static void test_native_stage_replay(void)
{
   cJSON *native = cJSON_Parse(REQ);
   assert(native);
   cJSON *system = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(native, "system"), 0);
   cJSON *cache = cJSON_AddObjectToObject(system, "cache_control");
   cJSON_AddStringToObject(cache, "type", "ephemeral");
   cJSON_AddStringToObject(cache, "ttl", "1h");
   char *frame = cJSON_PrintUnformatted(native);
   assert(frame);
   ir_seam_test_session("native-cache-stage");
   ir_seam_test_memory(1);
   ir_seam_test_persona("NATIVE_STAGE_PERSONA");
   for (int enabled = 0; enabled < 2; enabled++)
   {
      aimee_request_t request = {0};
      char error[128];
      ir_seam_test_native_preinject(enabled);
      assert(!aimee_frontend_parse_native_frame(native, frame, strlen(frame), AIMEE_WIRE_ANTHROPIC,
                                                &request, error, sizeof error));
      aimee_ir_apply_native_request_stages(&request);
      assert(ir_seam_test_plan_calls("context") == enabled);
      assert(ir_seam_test_plan_calls("tools") == enabled);
      char *wire = aimee_backend_render_wire(&request, AIMEE_WIRE_ANTHROPIC);
      assert(wire && strstr(wire, "\"ttl\":\"1h\""));
      if (enabled)
         assert(strstr(wire, "NATIVE_STAGE_PERSONA") && !strcmp(ir_seam_test_query(), "hi"));
      else
         assert(!strcmp(wire, frame));
      free(wire);
      aimee_request_free(&request);
   }
   ir_seam_test_native_preinject(0);
   ir_seam_test_memory(0);
   ir_seam_test_persona(NULL);
   ir_seam_test_session(NULL);
   cJSON_Delete(native);
   free(frame);
}

int main(void)
{
   test_native_stage_replay();
   printf("ir-serve: ");
   test_import_history();
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

   /* Invalid entry refuses; there is no legacy execution fallback. */
   assert(aimee_ir_build_provider_body(NULL, "openai", "m", 0, 1) == NULL);
   cJSON_Delete(req);

   /* Native Responses enters canonical ownership once and stays there through host stages. */
   const char *body = "{\"model\":\"native-model\",\"stream\":true,\"instructions\":\"be helpful\","
                      "\"input\":[{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":"
                      "\"input_text\",\"text\":\"hi\"}]}],"
                      "\"tools\":[{\"type\":\"function\",\"name\":\"Read\",\"strict\":true,"
                      "\"parameters\":{\"type\":\"object\"}}],"
                      "\"reasoning\":{\"effort\":\"high\"},\"text\":{\"format\":{\"type\":\"json_"
                      "schema\",\"name\":\"answer\",\"schema\":{\"type\":\"object\"}}}}";
   for (int provider = 0; provider < 3; provider++)
   {
      aimee_request_t canonical = {0};
      char error[128];
      ir_seam_test_session("responses-stage-once");
      ir_seam_test_persona("PERSONA_BOUNDARY_FIXTURE");
      ir_seam_test_memory(1);
      assert(!aimee_frontend_parse_wire(body, strlen(body), AIMEE_WIRE_RESPONSES, &canonical, error,
                                        sizeof error));
      assert(!aimee_ir_request_admit(&canonical));
      assert(ir_seam_test_plan_calls("context") == 0 && ir_seam_test_plan_calls("tools") == 0);
      assert(!strcmp(canonical.messages[0].blocks[0].text, "hi"));
      aimee_ir_apply_gateway_request_stages(&canonical);
      assert(ir_seam_test_plan_calls("context") == 1 && ir_seam_test_plan_calls("tools") == 1);
      assert(!strcmp(ir_seam_test_query(), "hi"));
      assert(ir_seam_test_guidance_provided() == 1);
      cJSON *native = provider == 0   ? openai_backend_build(&canonical)
                      : provider == 1 ? anthropic_backend_build(&canonical)
                                      : responses_backend_build(&canonical);
      char *wire = native ? cJSON_PrintUnformatted(native) : NULL;
      assert(wire && strstr(wire, "be helpful"));
      const char *persona = strstr(wire, "PERSONA_BOUNDARY_FIXTURE");
      assert(persona && !strstr(persona + 1, "PERSONA_BOUNDARY_FIXTURE"));
      assert(canonical.reasoning_effort == AIMEE_EFFORT_HIGH &&
             canonical.output_format.kind == AIMEE_OUTPUT_JSON_SCHEMA);
      free(wire);
      cJSON_Delete(native);
      aimee_request_free(&canonical);
   }
   ir_seam_test_memory(0);
   ir_seam_test_persona(NULL);
   ir_seam_test_session(NULL);

   /* A foreign canonical request keeps Unicode policy in the native system field. */
   const char *policy_body =
       "{\"model\":\"fixture-anthropic\",\"max_tokens\":32,\"messages\":["
       "{\"role\":\"system\",\"content\":\"Do not exceed LIMIT_7; preserve αβ🦊 and "
       "identifiers.\"},"
       "{\"role\":\"user\",\"content\":\"hi\"}],\"tools\":[{\"type\":\"function\",\"function\":{"
       "\"name\":\"Read\",\"parameters\":{\"type\":\"object\"}}}]}";
   aimee_request_t policy_request = {0};
   char policy_error[128];
   assert(!aimee_frontend_parse_wire(policy_body, strlen(policy_body), AIMEE_WIRE_OPENAI_CHAT,
                                     &policy_request, policy_error, sizeof policy_error));
   cJSON *policy_wire = anthropic_backend_build(&policy_request);
   assert(policy_wire);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(
                      cJSON_GetArrayItem(cJSON_GetObjectItem(policy_wire, "system"), 0), "text")),
                  "Do not exceed LIMIT_7; preserve αβ🦊 and identifiers."));
   assert(cJSON_IsObject(cJSON_GetObjectItem(
       cJSON_GetArrayItem(cJSON_GetObjectItem(policy_wire, "tools"), 0), "input_schema")));
   cJSON_Delete(policy_wire);
   aimee_request_free(&policy_request);

   /* LEGACY FLAT PATH: an already-established conversation must be terminal in
    * the durable delivery state. Otherwise a later compacted request, whose
    * assistant history has disappeared, looks like a fresh session and receives
    * a persona midway through it after a restart. */
   {
      ir_seam_test_session("legacy-established");
      ir_seam_test_persona("PERSONA-BODY <aimee-persona name=x>");

      char *established = aimee_ir_prepend_active_persona_text("follow-up", 1);
      assert(established && !strstr(established, "aimee-persona"));
      free(established);

      char *compacted = aimee_ir_prepend_active_persona_text("compacted follow-up", 0);
      assert(compacted && !strstr(compacted, "aimee-persona"));
      free(compacted);

      /* A genuinely fresh session does receive it, exactly once. */
      ir_seam_test_session("legacy-fresh");
      char *first = aimee_ir_prepend_active_persona_text("hello", 0);
      assert(first && strstr(first, "aimee-persona"));
      free(first);
      char *second = aimee_ir_prepend_active_persona_text("hello again", 0);
      assert(second && !strstr(second, "aimee-persona"));
      free(second);
   }

   printf("ok\n");
   return 0;
}
