#include "agent_response_projection.h"
#include <aimee/ir/session_event.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include <aimee/delegates/delegate_driver.h>
#include "agent_types.h"
#include <aimee/tools/agent_tools.h>
#include "cJSON.h"
#include "support/ir_response_fixture.h"

static void test_registry_init(void)
{
   /* init is idempotent */
   delegate_drivers_init();
   delegate_drivers_init();

   /* Built-in providers must be registered */
   assert(delegate_driver_get("openai") != NULL);
   assert(delegate_driver_get("anthropic") != NULL);
   assert(delegate_driver_get("chatgpt") != NULL);
   assert(delegate_driver_get("gemini") != NULL); /* -> openai via fallback */
   assert(delegate_driver_get("mistral") != NULL);

   /* Ollama maps to the OpenAI-compatible driver */
   const delegate_driver_t *ollama = delegate_driver_get("ollama");
   const delegate_driver_t *openai = delegate_driver_get("openai");
   assert(ollama != NULL);
   assert(strcmp(ollama->name, "openai") == 0);
   assert(ollama == openai);

   /* Legacy local delegate provider labels map explicitly, not by unknown fallback. */
   const delegate_driver_t *llama_eval = delegate_driver_get("llama-eval");
   const delegate_driver_t *llama_native = delegate_driver_get("llama_native");
   assert(llama_eval != NULL);
   assert(llama_native != NULL);
   assert(strcmp(llama_eval->name, "openai") == 0);
   assert(strcmp(llama_native->name, "openai") == 0);
   assert(llama_eval == openai);
   assert(llama_native == openai);

   /* Codex maps to the Responses/Codex driver */
   const delegate_driver_t *codex = delegate_driver_get("codex");
   assert(codex != NULL);
   assert(strcmp(codex->name, "chatgpt") == 0);

   /* Unknown provider falls back to openai */
   const delegate_driver_t *unk = delegate_driver_get("totally-unknown-provider");
   assert(unk != NULL);
   assert(strcmp(unk->name, "openai") == 0);

   /* NULL/empty provider falls back to openai */
   assert(delegate_driver_get(NULL) != NULL);
   assert(delegate_driver_get("") != NULL);
}

static void test_driver_names(void)
{
   delegate_drivers_init();

   const delegate_driver_t *d;

   d = delegate_driver_get("openai");
   assert(strcmp(d->name, "openai") == 0);

   d = delegate_driver_get("anthropic");
   assert(strcmp(d->name, "anthropic") == 0);

   d = delegate_driver_get("chatgpt");
   assert(strcmp(d->name, "chatgpt") == 0);

   /* gemini has no driver of its own any more: it speaks the OpenAI shape, so the
    * unknown-provider fallback hands it the openai driver. Pinned by identity
    * rather than by "not NULL" -- the fallback returns openai for ANY unknown
    * name, so != NULL would pass even if the routing were wrong. */
   d = delegate_driver_get("gemini");
   assert(d != NULL && strcmp(d->name, "openai") == 0);

   d = delegate_driver_get("mistral");
   assert(strcmp(d->name, "mistral") == 0);
}

static void test_build_url(void)
{
   delegate_drivers_init();

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.endpoint, sizeof(agent.endpoint), "https://api.openai.com/v1");
   snprintf(agent.model, sizeof(agent.model), "gpt-4o");

   char url[512];

   /* OpenAI: .../chat/completions */
   const delegate_driver_t *d = delegate_driver_get("openai");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "https://api.openai.com/v1/chat/completions") == 0);
   assert(strstr(url, "chat/completions") != NULL);

   /* Host root should normalize to /v1/chat/completions */
   snprintf(agent.endpoint, sizeof(agent.endpoint), "http://192.168.1.122:8080");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "http://192.168.1.122:8080/v1/chat/completions") == 0);

   /* Trailing slash should not produce a double slash */
   snprintf(agent.endpoint, sizeof(agent.endpoint), "http://192.168.1.122:8080/v1/");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "http://192.168.1.122:8080/v1/chat/completions") == 0);

   /* Anthropic: .../messages */
   snprintf(agent.endpoint, sizeof(agent.endpoint), "https://api.anthropic.com/v1");
   d = delegate_driver_get("anthropic");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "https://api.anthropic.com/v1/messages") == 0);
   assert(strstr(url, "messages") != NULL);

   /* Anthropic host root normalizes to /v1/messages */
   snprintf(agent.endpoint, sizeof(agent.endpoint), "https://api.anthropic.com");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "https://api.anthropic.com/v1/messages") == 0);

   /* Anthropic-compatible gateway with a path-prefixed base: /v1 must still be
    * inserted (regression: this previously produced .../anthropic/messages -> 404). */
   snprintf(agent.endpoint, sizeof(agent.endpoint), "https://api.minimax.io/anthropic");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "https://api.minimax.io/anthropic/v1/messages") == 0);

   /* A path-prefixed base that already carries /v1 stays idempotent (no double /v1). */
   snprintf(agent.endpoint, sizeof(agent.endpoint), "https://api.minimax.io/anthropic/v1");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "https://api.minimax.io/anthropic/v1/messages") == 0);

   /* An endpoint already pointing at .../messages is left untouched. */
   snprintf(agent.endpoint, sizeof(agent.endpoint), "https://api.minimax.io/anthropic/v1/messages");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "https://api.minimax.io/anthropic/v1/messages") == 0);

   /* ChatGPT: .../responses */
   snprintf(agent.endpoint, sizeof(agent.endpoint), "https://api.openai.com/v1");
   d = delegate_driver_get("chatgpt");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "https://api.openai.com/v1/responses") == 0);
   assert(strstr(url, "responses") != NULL);

   /* Codex alias: .../responses */
   snprintf(agent.endpoint, sizeof(agent.endpoint), "https://chatgpt.com/backend-api/codex");
   d = delegate_driver_get("codex");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "https://chatgpt.com/backend-api/codex/responses") == 0);

   /* Mistral: OpenAI-compatible .../chat/completions */
   snprintf(agent.endpoint, sizeof(agent.endpoint), "https://api.mistral.ai/v1");
   d = delegate_driver_get("mistral");
   assert(delegate_build_url(d, &agent, url, sizeof(url)) == 0);
   assert(strcmp(url, "https://api.mistral.ai/v1/chat/completions") == 0);
}

static void test_get_caps(void)
{
   delegate_drivers_init();

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.model, sizeof(agent.model), "gpt-4o");

   driver_caps_t caps;

   /* OpenAI */
   const delegate_driver_t *d = delegate_driver_get("openai");
   delegate_get_caps(d, &agent, &caps);
   assert(caps.capability_flags & DRIVER_CAP_TOOL_CALLS);
   assert(caps.context_limit > 0);

   /* Anthropic */
   snprintf(agent.model, sizeof(agent.model), "claude-sonnet-4-6");
   d = delegate_driver_get("anthropic");
   delegate_get_caps(d, &agent, &caps);
   assert(caps.capability_flags & DRIVER_CAP_TOOL_CALLS);
   assert(caps.capability_flags & DRIVER_CAP_SYSTEM_MSG);

   /* NULL driver: defaults */
   delegate_get_caps(NULL, &agent, &caps);
   assert(caps.capability_flags & DRIVER_CAP_TOOL_CALLS);
   assert(caps.context_limit == DRIVER_CTX_LARGE);
}

static void test_delegate_respond_spec_shape(void)
{
   cJSON *tools = build_tools_array();
   assert(tools != NULL);
   int before = cJSON_GetArraySize(tools);
   assert(agent_tools_append_delegate_respond_tool(tools) == 1);
   assert(cJSON_GetArraySize(tools) == before + 1);
   assert(agent_tools_append_delegate_respond_tool(tools) == 0);
   assert(cJSON_GetArraySize(tools) == before + 1);

   cJSON *respond = cJSON_GetArrayItem(tools, before);
   cJSON *fn = cJSON_GetObjectItem(respond, "function");
   cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
   cJSON *params = fn ? cJSON_GetObjectItem(fn, "parameters") : NULL;
   cJSON *props = params ? cJSON_GetObjectItem(params, "properties") : NULL;
   cJSON *message = props ? cJSON_GetObjectItem(props, "message") : NULL;
   assert(cJSON_IsString(name) && strcmp(name->valuestring, "respond") == 0);
   assert(cJSON_IsObject(params));
   assert(cJSON_IsObject(message));

   cJSON_Delete(tools);
}

static void check_delegate_respond(const char *respond_id, const char *real_id, const char *answer)
{
   aimee_response_t response = {0};
   response.n_content = real_id ? 2 : 1;
   response.content = calloc((size_t)response.n_content, sizeof *response.content);
   assert(response.content);
   response.content[0] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                         .tool_name = strdup("respond"),
                                         .tool_id = strdup(respond_id),
                                         .tool_input = cJSON_CreateObject()};
   assert(cJSON_AddStringToObject(response.content[0].tool_input, "message", answer));
   if (real_id)
      response.content[1] = (aimee_block_t){
          .type = AIMEE_BLK_TOOL_USE,
          .tool_name = strdup("bash"),
          .tool_id = strdup(real_id),
          .tool_input = cJSON_Parse("{\"command\":\"pwd\",\"exact\":9007199254740993}")};
   assert(agent_ir_strip_delegate_respond(&response) == (real_id ? 2 : 1));
   if (!real_id)
   {
      assert(!aimee_ir_response_has_tool_use(&response));
      assert_ir_text(&response, answer);
   }
   else
   {
      assert(aimee_ir_response_tool_count(&response) == 1);
      const aimee_block_t *tool = aimee_ir_response_tool(&response, 0);
      assert(!strcmp(tool->tool_id, real_id) && !strcmp(tool->tool_name, "bash"));
      char *input = cJSON_PrintUnformatted(tool->tool_input);
      assert(input && !strcmp(input, "{\"command\":\"pwd\",\"exact\":9007199254740993}"));
      free(input);
   }
   aimee_response_free(&response);
}

static void test_delegate_respond_strip_pure(void)
{
   check_delegate_respond("call_1", NULL, "done");
}
static void test_delegate_respond_strip_mixed(void)
{
   check_delegate_respond("call_1", "call_2", "checking");
}
static void test_delegate_respond_rescue_then_strip_mixed(void)
{
   check_delegate_respond("xml_call_1", "xml_call_2", "checking");
}
static void test_delegate_respond_xml_rescue_then_strip_pure(void)
{
   check_delegate_respond("xml_call_1", NULL, "final answer");
}

static void test_respond_retains_native_stop_metadata(void)
{
   const aimee_stop_reason_t stops[] = {AIMEE_STOP_TOOL_USE, AIMEE_STOP_MAX_TOKENS,
                                        AIMEE_STOP_STOP_SEQUENCE, AIMEE_STOP_UNKNOWN};
   char reason[301];
   memset(reason, 'r', sizeof reason - 1);
   memcpy(reason, "native-reason:", 14);
   reason[sizeof reason - 1] = 0;
   for (size_t i = 0; i < sizeof stops / sizeof stops[0]; i++)
   {
      aimee_response_t response = {.stop_reason = stops[i], .n_content = 1};
      response.content = calloc(1, sizeof *response.content);
      assert(response.content);
      response.content[0] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                            .tool_name = strdup("respond"),
                                            .tool_id = strdup("final"),
                                            .tool_input = cJSON_Parse("{\"message\":\"answer\"}")};
      response.raw_stop_reason = strdup(reason);
      assert(agent_ir_strip_delegate_respond(&response) == 1);
      if (stops[i] == AIMEE_STOP_TOOL_USE)
         assert(response.stop_reason == AIMEE_STOP_END_TURN && !response.raw_stop_reason);
      else
         assert(response.stop_reason == stops[i] && response.raw_stop_reason &&
                !strcmp(response.raw_stop_reason, reason));
      assert_ir_text(&response, "answer");
      aimee_response_free(&response);
   }
}

static int final_results;
static int capture_final(const aimee_session_event_t *event, void *context)
{
   int mixed = *(int *)context;
   const aimee_block_t *block = &event->message->blocks[0];
   assert(event->kind == AIMEE_SESSION_MESSAGE && block->type == AIMEE_BLK_TOOL_RESULT);
   assert(!strcmp(block->tool_id, "respond-id") && block->tool_is_error == mixed);
   if (!mixed)
      assert(!strcmp(cJSON_GetStringValue(block->tool_result), "answer"));
   final_results++;
   return 0;
}
static void test_typed_delegate_respond(void)
{
   for (int mixed = 0; mixed <= 1; mixed++)
   {
      aimee_response_t response = {0};
      response.n_content = mixed ? 5 : 4;
      response.content = calloc((size_t)response.n_content, sizeof *response.content);
      assert(response.content);
      response.content[0] = (aimee_block_t){.type = AIMEE_BLK_THINKING,
                                            .text = strdup("reason"),
                                            .thinking_signature = strdup("sig")};
      response.content[1] = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup("preamble")};
      response.content[2] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                            .tool_id = strdup("respond-id"),
                                            .tool_name = strdup("respond"),
                                            .tool_input = cJSON_Parse("{\"message\":\"answer\"}")};
      response.content[3] = (aimee_block_t){
          .type = AIMEE_BLK_IMAGE, .media_type = strdup("image/png"), .media_ref = strdup("AA==")};
      if (mixed)
         response.content[4] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                               .tool_id = strdup("keep-id"),
                                               .tool_name = strdup("read_file"),
                                               .tool_input = cJSON_Parse("{\"path\":\"a\"}")};
      response.stop_reason = AIMEE_STOP_TOOL_USE;
      aimee_ir_session_set_sink(capture_final, &mixed);
      assert(agent_response_consume_final(&response) == (mixed ? 2 : 1));
      aimee_ir_session_set_sink(NULL, NULL);
      assert(final_results == mixed + 1);
      assert(!strcmp(response.content[0].thinking_signature, "sig"));
      assert(response.content[mixed ? 2 : 2].type == AIMEE_BLK_IMAGE);
      if (mixed)
      {
         assert(response.n_content == 4 && aimee_ir_response_tool_count(&response) == 1);
         assert(!strcmp(aimee_ir_response_tool(&response, 0)->tool_id, "keep-id"));
         assert(!strcmp(response.content[1].text, "preamble"));
      }
      else
      {
         assert(response.n_content == 3 && aimee_ir_response_tool_count(&response) == 0);
         assert(!strcmp(response.content[1].text, "answer"));
         assert(response.stop_reason == AIMEE_STOP_END_TURN);
      }
      aimee_response_free(&response);
   }
}

static int capture_local_final_only(const aimee_session_event_t *event, void *context)
{
   assert(event->kind == AIMEE_SESSION_MESSAGE && event->message->n_blocks == 1);
   const aimee_block_t *block = &event->message->blocks[0];
   assert(block->type == AIMEE_BLK_TOOL_RESULT && block->tool_is_error);
   assert(!strcmp(block->tool_id, "respond-id"));
   (*(int *)context)++;
   return 0;
}
static void test_namespaced_respond_is_not_host_final(void)
{
   aimee_response_t response = {.n_content = 1, .stop_reason = AIMEE_STOP_TOOL_USE};
   response.content = calloc(1, sizeof *response.content);
   assert(response.content);
   response.content[0] =
       (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                       .tool_name = strdup("respond"),
                       .tool_namespace = strdup("foreign"),
                       .tool_id = strdup("foreign-id"),
                       .tool_input = cJSON_Parse("{\"message\":\"not a final answer\"}")};
   int emitted = 0;
   aimee_ir_session_set_sink(capture_local_final_only, &emitted);
   assert(agent_response_consume_final(&response) == 0 && !emitted);
   aimee_ir_session_set_sink(NULL, NULL);
   assert(response.n_content == 1 && response.stop_reason == AIMEE_STOP_TOOL_USE);
   assert(!strcmp(response.content[0].tool_namespace, "foreign"));
   assert(!strcmp(response.content[0].tool_id, "foreign-id"));
   response.content = realloc(response.content, 2 * sizeof *response.content);
   assert(response.content);
   response.n_content = 2;
   response.content[1] =
       (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                       .tool_name = strdup("respond"),
                       .tool_id = strdup("respond-id"),
                       .tool_input = cJSON_Parse("{\"message\":\"local answer\"}")};
   aimee_ir_session_set_sink(capture_local_final_only, &emitted);
   assert(agent_response_consume_final(&response) == 2 && emitted == 1);
   aimee_ir_session_set_sink(NULL, NULL);
   assert(response.n_content == 1 && !strcmp(response.content[0].tool_namespace, "foreign"));
   assert(!strcmp(response.content[0].tool_id, "foreign-id"));
   aimee_response_free(&response);
}

int main(void)
{
   printf("delegate_driver: ");
   test_namespaced_respond_is_not_host_final();
   test_typed_delegate_respond();
   test_respond_retains_native_stop_metadata();
   test_registry_init();
   printf("registry OK, ");
   test_driver_names();
   printf("names OK, ");
   test_build_url();
   printf("build_url OK, ");
   test_get_caps();
   printf("get_caps OK, ");
   test_delegate_respond_spec_shape();
   printf("respond_spec OK, ");
   test_delegate_respond_strip_pure();
   printf("respond_strip_pure OK, ");
   test_delegate_respond_strip_mixed();
   printf("respond_strip_mixed OK, ");
   test_delegate_respond_rescue_then_strip_mixed();
   printf("respond_rescue_strip_mixed OK\n");
   test_delegate_respond_xml_rescue_then_strip_pure();
   printf("respond_xml_rescue_strip_pure OK\n");
   return 0;
}
