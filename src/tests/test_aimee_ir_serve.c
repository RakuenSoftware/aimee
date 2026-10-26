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
   /* Provider codecs consume the already-owned request. The retired provider
    * body shim is not a production path and must not be what this test proves. */
   for (int provider = 0; provider < 2; provider++)
      for (int stream = 0; stream < 2; stream++)
      {
         aimee_request_t ir = {0};
         char error[128];
         assert(!aimee_frontend_parse_wire(REQ, strlen(REQ), AIMEE_WIRE_ANTHROPIC, &ir, error,
                                           sizeof error));
         free(ir.model);
         ir.model = strdup("served-model");
         ir.max_tokens = 200;
         ir.has_max_tokens = 1;
         ir.stream = stream;
         int control_losses = 0;
         char *wire =
             provider ? aimee_backend_render_wire_profile(&ir, AIMEE_WIRE_RESPONSES,
                                                          AIMEE_RESPONSES_CODEX, &control_losses)
                      : aimee_backend_render_wire(&ir, AIMEE_WIRE_OPENAI_CHAT);
         cJSON *body = wire ? cJSON_Parse(wire) : NULL;
         assert(body &&
                !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "model")), "served-model"));
         if (provider)
            assert(cJSON_IsTrue(cJSON_GetObjectItem(body, "stream"))); /* Codex requires SSE. */
         else
            assert(stream ? cJSON_IsTrue(cJSON_GetObjectItem(body, "stream"))
                          : !cJSON_GetObjectItem(body, "stream"));
         assert(cJSON_IsArray(cJSON_GetObjectItem(body, provider ? "input" : "messages")));
         assert(cJSON_IsArray(cJSON_GetObjectItem(body, "tools")));
         if (provider)
         {
            assert(cJSON_IsFalse(cJSON_GetObjectItem(body, "store")));
            assert(!cJSON_GetObjectItem(body, "max_output_tokens"));
            assert(control_losses == 1 && ir.has_max_tokens && ir.max_tokens == 200);
            assert(cJSON_GetObjectItem(body, "instructions"));
         }
         else
            assert(cJSON_GetObjectItem(body, "max_tokens")->valueint == 200);
         cJSON_Delete(body);
         free(wire);
         aimee_request_free(&ir);
      }

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

   /* The active typed owner must remember an established conversation in
    * the durable delivery state. Otherwise a later compacted request, whose
    * assistant history has disappeared, looks like a fresh session and receives
    * a persona midway through it after a restart. */
   {
      ir_seam_test_session("typed-established");
      ir_seam_test_persona("PERSONA-BODY <aimee-persona name=x>");
      const char *established =
          "{\"model\":\"m\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\",\"content\":"
          "\"previous\"},{\"role\":\"assistant\",\"content\":\"prior "
          "answer\"},{\"role\":\"user\",\"content\":\"follow-up\"}]}";
      const char *opening = "{\"model\":\"m\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\","
                            "\"content\":\"hello\"}]}";
      for (int phase = 0; phase < 4; phase++)
      {
         if (phase == 2)
            ir_seam_test_session("typed-fresh");
         const char *input = phase == 0 ? established : opening;
         aimee_request_t request = {0};
         char error[128];
         assert(!aimee_frontend_parse_wire(input, strlen(input), AIMEE_WIRE_ANTHROPIC, &request,
                                           error, sizeof error));
         aimee_ir_apply_gateway_request_stages(&request);
         char *wire = aimee_backend_render_wire(&request, AIMEE_WIRE_ANTHROPIC);
         assert(wire);
         assert((strstr(wire, "PERSONA-BODY") != NULL) == (phase == 2));
         free(wire);
         aimee_request_free(&request);
      }
      ir_seam_test_session(NULL);
      ir_seam_test_persona(NULL);
   }

   printf("ok\n");
   return 0;
}
