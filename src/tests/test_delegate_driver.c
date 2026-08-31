#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include <aimee/delegates/delegate_driver.h>
#include "agent_types.h"
#include <aimee/tools/agent_tools.h>
#include "cJSON.h"

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

static void test_delegate_respond_strip_pure(void)
{
   parsed_response_t pr;
   memset(&pr, 0, sizeof(pr));
   pr.is_tool_call = 1;
   pr.call_count = 1;
   snprintf(pr.calls[0].id, sizeof(pr.calls[0].id), "call_1");
   snprintf(pr.calls[0].name, sizeof(pr.calls[0].name), "respond");
   pr.calls[0].arguments = safe_strdup("{\"message\":\"done\"}");

   assert(agent_tools_strip_delegate_respond(&pr) == 1);
   assert(pr.is_tool_call == 0);
   assert(pr.call_count == 0);
   assert(pr.content != NULL);
   assert(strcmp(pr.content, "done") == 0);

   agent_free_parsed_response(&pr);
}

static void test_delegate_respond_strip_mixed(void)
{
   parsed_response_t pr;
   memset(&pr, 0, sizeof(pr));
   pr.is_tool_call = 1;
   pr.call_count = 2;
   snprintf(pr.calls[0].id, sizeof(pr.calls[0].id), "call_1");
   snprintf(pr.calls[0].name, sizeof(pr.calls[0].name), "respond");
   pr.calls[0].arguments = safe_strdup("{\"message\":\"checking\"}");
   snprintf(pr.calls[1].id, sizeof(pr.calls[1].id), "call_2");
   snprintf(pr.calls[1].name, sizeof(pr.calls[1].name), "bash");
   pr.calls[1].arguments = safe_strdup("{\"command\":\"pwd\"}");

   assert(agent_tools_strip_delegate_respond(&pr) == 2);
   assert(pr.is_tool_call == 1);
   assert(pr.call_count == 1);
   assert(strcmp(pr.calls[0].id, "call_2") == 0);
   assert(strcmp(pr.calls[0].name, "bash") == 0);
   assert(strcmp(pr.calls[0].arguments, "{\"command\":\"pwd\"}") == 0);

   agent_free_parsed_response(&pr);
}

static void test_delegate_respond_rescue_then_strip_mixed(void)
{
   /* The subject is the STRIP: a respond call alongside a real tool call is
    * removed, and the real call survives untouched. Where those two calls came
    * from is not this test's business, so it states them directly. */
   parsed_response_t pr;
   memset(&pr, 0, sizeof(pr));
   pr.is_tool_call = 1;
   pr.call_count = 2;
   snprintf(pr.calls[0].id, sizeof(pr.calls[0].id), "%s", "xml_call_1");
   snprintf(pr.calls[0].name, sizeof(pr.calls[0].name), "%s", "respond");
   pr.calls[0].arguments = strdup("{\"message\":\"checking\"}");
   snprintf(pr.calls[1].id, sizeof(pr.calls[1].id), "%s", "xml_call_2");
   snprintf(pr.calls[1].name, sizeof(pr.calls[1].name), "%s", "bash");
   pr.calls[1].arguments = strdup("{\"command\":\"pwd\"}");

   assert(agent_tools_strip_delegate_respond(&pr) == 2);
   assert(pr.is_tool_call == 1);
   assert(pr.call_count == 1);
   assert(strcmp(pr.calls[0].name, "bash") == 0);
   assert(strcmp(pr.calls[0].arguments, "{\"command\":\"pwd\"}") == 0);

   agent_free_parsed_response(&pr);
}

static void test_delegate_respond_xml_rescue_then_strip_pure(void)
{
   /* A turn that is ONLY a respond call stops being a tool turn at all, and the
    * message becomes the content the caller answers with. */
   parsed_response_t pr;
   memset(&pr, 0, sizeof(pr));
   pr.is_tool_call = 1;
   pr.call_count = 1;
   snprintf(pr.calls[0].id, sizeof(pr.calls[0].id), "%s", "xml_call_1");
   snprintf(pr.calls[0].name, sizeof(pr.calls[0].name), "%s", "respond");
   pr.calls[0].arguments = strdup("{\"message\":\"final answer\"}");

   assert(agent_tools_strip_delegate_respond(&pr) == 1);
   assert(pr.is_tool_call == 0);
   assert(pr.content != NULL);
   assert(strcmp(pr.content, "final answer") == 0);

   agent_free_parsed_response(&pr);
}

int main(void)
{
   printf("delegate_driver: ");
   test_registry_init();
   printf("registry OK, ");
   test_driver_names();
   printf("names OK, ");
   test_build_url();
   printf("build_url OK, ");
   test_get_caps();
   printf("get_caps OK, ");
   printf("xml_fallback OK, ");
   printf("channel_fallback OK, ");
   printf("qwen_xml_fallback OK, ");
   printf("mistral_bracket_fallback OK, ");
   printf("minimax_invoke_fallback OK, ");
   printf("bare_json_fallback OK, ");
   printf("unfenced_json_fallback OK, ");
   printf("unknown_json_rejected OK, ");
   printf("json_disable_gate OK, ");
   printf("respond_json_rescue OK, ");
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
