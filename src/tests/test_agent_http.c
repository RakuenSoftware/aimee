/* test_agent_http.c: unit tests for the agent HTTP request/response shapes
 * (OpenAI chat, Anthropic messages, Responses) and the model registry.
 *
 * Was originally the Gemini prompt-cache suite; Gemini is reached through its
 * OpenAI-compatible endpoint now, so the bespoke Gemini paths and their tests are
 * gone. A `google/gemini-*` entry survives in the OpenRouter model-list case —
 * that is a MODEL served over the OpenAI shape, not a Gemini protocol. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_protocol.h"
#include <aimee/gateway/gateway_policy.h>
#include <aimee/delegates/delegate_driver.h>
#include <aimee/delegates/delegate_xml_fallback.h>
#include "model_provider.h"
#include "model_sampling.h"
#include "platform_test_util.h"
#include "cJSON.h"
#include "support/ir_response_fixture.h"
#include <aimee/ir/session_event.h>
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>

#include "support/typed_catalog_fixture.h"
static aimee_message_t fixture_context(const char *system)
{
   aimee_message_t context = {.role = strdup("system")};
   assert(context.role);
   if (system && system[0])
   {
      context.blocks = calloc(1, sizeof *context.blocks);
      assert(context.blocks);
      context.n_blocks = 1;
      context.blocks[0] = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup(system)};
      assert(context.blocks[0].text);
      aimee_ir_block_set_context(context.blocks, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                                 AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                                 0);
   }
   return context;
}
static cJSON *fixture_build_catalog(const agent_t *agent, const cJSON *messages,
                                    const aimee_request_t *catalog, const char *system, int maximum,
                                    double temperature, aimee_wire_t wire, int delegate,
                                    aimee_request_t *dispatch)
{
   aimee_message_t context = fixture_context(system);
   cJSON *result = agent_build_seeded_request_ir(agent, NULL, messages, catalog, &context, maximum,
                                                 temperature, wire, delegate, 1, dispatch);
   aimee_message_free(&context);
   return result;
}
static int fixture_prepare_catalog(const agent_t *agent, const cJSON *messages,
                                   const aimee_request_t *catalog, const char *system, int maximum,
                                   double temperature, aimee_wire_t wire, int delegate,
                                   aimee_request_t *out)
{
   aimee_message_t context = fixture_context(system);
   int result = agent_prepare_seeded_request_ir(agent, NULL, messages, catalog, &context, maximum,
                                                temperature, wire, delegate, out);
   aimee_message_free(&context);
   return result;
}
static cJSON *fixture_build_request(const agent_t *agent, const cJSON *messages, cJSON *tools,
                                    const char *system, int maximum, double temperature,
                                    aimee_wire_t wire, int delegate, aimee_request_t *dispatch)
{
   aimee_request_t catalog = {0};
   if (fixture_catalog_import(tools, wire, &catalog))
      return NULL;
   cJSON *result = fixture_build_catalog(agent, messages, &catalog, system, maximum, temperature,
                                         wire, delegate, dispatch);
   aimee_request_free(&catalog);
   return result;
}
static int fixture_prepare_request(const agent_t *agent, const cJSON *messages, cJSON *tools,
                                   const char *system, int maximum, double temperature,
                                   aimee_wire_t wire, int delegate, aimee_request_t *out)
{
   aimee_request_t catalog = {0};
   if (fixture_catalog_import(tools, wire, &catalog))
      return -1;
   int result = fixture_prepare_catalog(agent, messages, &catalog, system, maximum, temperature,
                                        wire, delegate, out);
   aimee_request_free(&catalog);
   return result;
}
/* Native fixture data enters the same explicit history adapter as external clients. */
static cJSON *build_profile(const agent_t *agent, cJSON *messages, cJSON *tools, const char *system,
                            int maximum, double temperature, aimee_wire_t wire)
{
   cJSON *history = aimee_frontend_history_import(messages, wire);
   assert(history);
   cJSON *request =
       fixture_build_request(agent, history, tools, system, maximum, temperature, wire, 0, NULL);
   cJSON_Delete(history);
   return request;
}
static cJSON *build_chat_profile(const agent_t *agent, cJSON *messages, cJSON *tools, int maximum,
                                 double temperature)
{
   return build_profile(agent, messages, tools, NULL, maximum, temperature, AIMEE_WIRE_OPENAI_CHAT);
}
static cJSON *build_responses_profile(const agent_t *agent, cJSON *messages, cJSON *tools,
                                      const char *system)
{
   return build_profile(agent, messages, tools, system, 0, -1, AIMEE_WIRE_RESPONSES);
}
static cJSON *build_anthropic_profile(const agent_t *agent, cJSON *messages, cJSON *tools,
                                      const char *system, int maximum, double temperature)
{
   return build_profile(agent, messages, tools, system, maximum, temperature, AIMEE_WIRE_ANTHROPIC);
}

static cJSON *make_dummy_tool(void)
{
   cJSON *tools = cJSON_CreateArray();
   cJSON *tool = cJSON_CreateObject();
   cJSON_AddStringToObject(tool, "type", "function");
   cJSON *fn = cJSON_AddObjectToObject(tool, "function");
   cJSON_AddStringToObject(fn, "name", "read_file");
   cJSON_AddStringToObject(fn, "description", "Read a file");
   cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
   cJSON_AddStringToObject(params, "type", "object");
   cJSON_AddItemToArray(tools, tool);
   return tools;
}

static void test_build_request_openai_omits_empty_tools(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.model, sizeof(agent.model), "gpt-4o-mini");

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, 0.0);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "tools") == NULL);
   assert(cJSON_GetObjectItem(req, "parallel_tool_calls") == NULL);
   assert(cJSON_GetObjectItem(req, "chat_template_kwargs") == NULL);
   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("build_request_openai_omits_empty_tools OK\n");
}

static void test_build_request_openai_qwen_profile(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "llama-eval");
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "Qwen_Qwen3.6-35B-A3B-Q5_K_M.gguf");

   cJSON *messages = cJSON_CreateArray();
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", "hello");
   cJSON_AddItemToArray(messages, user);
   cJSON *tools = make_dummy_tool();
   cJSON *req = build_chat_profile(&agent, messages, tools, 1024, 0.0);
   assert(req != NULL);
   assert(cJSON_IsArray(cJSON_GetObjectItem(req, "tools")));
   cJSON *ptc = cJSON_GetObjectItem(req, "parallel_tool_calls");
   assert(cJSON_IsFalse(ptc));
   assert(cJSON_GetObjectItem(req, "chat_template_kwargs") == NULL);
   cJSON *req_messages = cJSON_GetObjectItem(req, "messages");
   cJSON *req_user = cJSON_GetArrayItem(req_messages, 0);
   const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(req_user, "content"));
   assert(content != NULL && strncmp(content, "/no_think\n", 10) == 0);
   cJSON_Delete(req);
   cJSON_Delete(tools);
   cJSON_Delete(messages);
   printf("build_request_openai_qwen_profile OK\n");
}

static void test_build_request_openai_qwen_no_tools_disables_thinking(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "llama-eval");
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "Qwen_Qwen3.6-35B-A3B-Q5_K_M.gguf");

   cJSON *messages = cJSON_CreateArray();
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", "hello");
   cJSON_AddItemToArray(messages, user);
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, 0.0);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "tools") == NULL);
   assert(cJSON_GetObjectItem(req, "parallel_tool_calls") == NULL);
   assert(cJSON_GetObjectItem(req, "chat_template_kwargs") == NULL);
   cJSON *req_messages = cJSON_GetObjectItem(req, "messages");
   cJSON *req_user = cJSON_GetArrayItem(req_messages, 0);
   const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(req_user, "content"));
   assert(content != NULL && strncmp(content, "/no_think\n", 10) == 0);
   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("build_request_openai_qwen_no_tools_disables_thinking OK\n");
}

static void test_build_request_openai_standard_tools(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "openai");
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "gpt-4o-mini");

   cJSON *messages = cJSON_CreateArray();
   cJSON *tools = make_dummy_tool();
   cJSON *req = build_chat_profile(&agent, messages, tools, 1024, 0.0);
   assert(req != NULL);
   assert(cJSON_IsArray(cJSON_GetObjectItem(req, "tools")));
   assert(cJSON_GetObjectItem(req, "parallel_tool_calls") == NULL);
   assert(cJSON_GetObjectItem(req, "chat_template_kwargs") == NULL);
   cJSON_Delete(req);
   cJSON_Delete(tools);
   cJSON_Delete(messages);
   printf("build_request_openai_standard_tools OK\n");
}

static void test_build_request_openrouter_routing_hint(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "openrouter");
   snprintf(agent.model, sizeof(agent.model), "anthropic/claude-opus-4.7");

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, 0.0);
   assert(req != NULL);

   cJSON *route = cJSON_GetObjectItem(req, "route");
   assert(route != NULL);
   assert(cJSON_IsString(route));
   assert(strcmp(route->valuestring, "fallback") == 0);

   cJSON *models = cJSON_GetObjectItem(req, "models");
   assert(models != NULL);
   assert(cJSON_IsArray(models));
   assert(cJSON_GetArraySize(models) == 3);
   assert(strcmp(cJSON_GetArrayItem(models, 0)->valuestring, "anthropic/claude-opus-4.7") == 0);
   assert(strcmp(cJSON_GetArrayItem(models, 1)->valuestring, "google/gemini-2.5-flash") == 0);
   assert(strcmp(cJSON_GetArrayItem(models, 2)->valuestring, "mistralai/mistral-large-2512") == 0);

   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("build_request_openrouter_routing_hint OK\n");
}

static void test_build_request_openai_mistral_vibe_options(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "mistral");
   snprintf(agent.model, sizeof(agent.model), "mistral-vibe-cli-latest");

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, 0.0);
   assert(req != NULL);

   cJSON *effort = cJSON_GetObjectItem(req, "reasoning_effort");
   assert(effort != NULL);
   assert(cJSON_IsString(effort));
   assert(strcmp(effort->valuestring, "high") == 0);

   cJSON *temp = cJSON_GetObjectItem(req, "temperature");
   assert(temp != NULL);
   assert(cJSON_IsNumber(temp));
   assert(temp->valuedouble == 1.0);

   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("build_request_openai_mistral_vibe_options OK\n");
}

static void test_build_request_openai_minimax_options(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "minimax");
   snprintf(agent.endpoint, sizeof(agent.endpoint), "https://api.minimax.io/v1");
   snprintf(agent.model, sizeof(agent.model), "MiniMax-M2.7");

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, 0.0);
   assert(req != NULL);

   cJSON *split = cJSON_GetObjectItem(req, "reasoning_split");
   assert(cJSON_IsTrue(split));

   cJSON_Delete(req);

   /* MiniMax-M3 (and any non-M2.7 MiniMax model) must NOT get reasoning_split —
    * the current MiniMax API rejects it with HTTP 400. */
   snprintf(agent.model, sizeof(agent.model), "MiniMax-M3");
   cJSON *req_m3 = build_chat_profile(&agent, messages, NULL, 1024, 0.0);
   assert(req_m3 != NULL);
   assert(cJSON_GetObjectItem(req_m3, "reasoning_split") == NULL);
   cJSON_Delete(req_m3);

   cJSON_Delete(messages);
   printf("build_request_openai_minimax_options OK\n");
}

static void test_model_sampling_lookup_qwen_stem(void)
{
   model_sampling_row_t row;
   assert(model_sampling_get("Qwen_Qwen3.6-35B-A3B-Q5_K_M.gguf", &row) == 1);
   assert(row.temperature == 0.6);
   assert(row.top_p == 0.95);
   assert(row.top_k == 20);
   assert(row.min_p == 0.0);
   printf("model_sampling_lookup_qwen_stem OK\n");
}

static void test_openai_recommended_sampling_applies_map(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "Qwen_Qwen3.6-35B-A3B-Q5_K_M.gguf");
   agent.recommended_sampling = 1;

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, -1);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "temperature")->valuedouble == 0.6);
   assert(cJSON_GetObjectItem(req, "top_p")->valuedouble == 0.95);
   assert(cJSON_GetObjectItem(req, "top_k")->valueint == 20);
   assert(cJSON_GetObjectItem(req, "min_p")->valuedouble == 0.0);
   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("openai_recommended_sampling_applies_map OK\n");
}

static void test_openai_recommended_sampling_ministral_temperature(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "ministral-3:8b-instruct-2512-q4_K_M");
   agent.recommended_sampling = 1;

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, -1);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "temperature")->valuedouble == 0.05);
   assert(cJSON_GetObjectItem(req, "top_p") == NULL);
   assert(cJSON_GetObjectItem(req, "top_k") == NULL);
   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("openai_recommended_sampling_ministral_temperature OK\n");
}

static void test_openai_sampling_caller_temperature_wins(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "ministral-3:8b-instruct-2512-q4_K_M");
   agent.recommended_sampling = 1;

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, 0.42);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "temperature")->valuedouble == 0.42);
   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("openai_sampling_caller_temperature_wins OK\n");
}

static void test_openai_sampling_opt_out_unchanged(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "Qwen_Qwen3.6-35B-A3B-Q5_K_M.gguf");

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, -1);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "temperature") == NULL);
   assert(cJSON_GetObjectItem(req, "top_p") == NULL);
   assert(cJSON_GetObjectItem(req, "top_k") == NULL);
   assert(cJSON_GetObjectItem(req, "min_p") == NULL);
   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("openai_sampling_opt_out_unchanged OK\n");
}

static void test_openai_sampling_unknown_opt_in_unchanged(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "unknown-local-model");
   agent.recommended_sampling = 1;

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, -1);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "temperature") == NULL);
   assert(cJSON_GetObjectItem(req, "top_p") == NULL);
   assert(cJSON_GetObjectItem(req, "top_k") == NULL);
   assert(cJSON_GetObjectItem(req, "min_p") == NULL);
   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("openai_sampling_unknown_opt_in_unchanged OK\n");
}

static void test_openai_provider_fixed_temperature_fallback(void)
{
   model_provider_t *provider = model_provider_get("openai");
   assert(provider != NULL);
   int old_fixed_temperature = provider->fixed_temperature;
   provider->fixed_temperature = 0;

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "unknown-local-model");

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, -1);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "temperature")->valuedouble == 0.0);
   cJSON_Delete(req);
   cJSON_Delete(messages);

   provider->fixed_temperature = old_fixed_temperature;
   printf("openai_provider_fixed_temperature_fallback OK\n");
}

/* A model that accepts exactly one temperature must get that value even when
 * the caller asked for another. kimi-k3 names no wire provider in agents.json,
 * so the constraint has to resolve off the catalog vendor; sending the caller's
 * temperature instead failed every delegate with HTTP 400 "invalid temperature:
 * only 1 is allowed for this model". */
static void test_openai_required_temperature_overrides_caller(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.catalog_provider, sizeof(agent.catalog_provider), "moonshotai");
   snprintf(agent.model, sizeof(agent.model), "k3-256k");
   assert(model_sampling_required_temperature(&agent) == 1.0);

   cJSON *messages = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, NULL, 1024, 0.2);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "temperature")->valuedouble == 1.0);
   cJSON_Delete(req);
   cJSON_Delete(messages);

   /* An unconstrained model still honours the caller. */
   agent_t other;
   memset(&other, 0, sizeof(other));
   snprintf(other.provider, sizeof(other.provider), "openai");
   snprintf(other.model, sizeof(other.model), "unknown-local-model");
   assert(model_sampling_required_temperature(&other) < 0);

   messages = cJSON_CreateArray();
   req = build_chat_profile(&other, messages, NULL, 1024, 0.2);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "temperature")->valuedouble == 0.2);
   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("openai_required_temperature_overrides_caller OK\n");
}

static void test_agent_config_recommended_sampling_roundtrip(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-sampling-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   char *old_aimee_home = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
   char *old_no_cache = getenv("AIMEE_NO_CACHE") ? strdup(getenv("AIMEE_NO_CACHE")) : NULL;
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");

   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 1;
   snprintf(cfg.default_agent, sizeof(cfg.default_agent), "local");
   agent_t *ag = &cfg.agents[0];
   snprintf(ag->name, sizeof(ag->name), "local");
   snprintf(ag->endpoint, sizeof(ag->endpoint), "http://127.0.0.1:8080/v1");
   snprintf(ag->model, sizeof(ag->model), "Qwen_Qwen3.6-35B-A3B-Q5_K_M.gguf");
   snprintf(ag->provider, sizeof(ag->provider), "openai");
   snprintf(ag->auth_type, sizeof(ag->auth_type), "none");
   ag->enabled = 1;
   ag->recommended_sampling = 1;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = AGENT_DEFAULT_TIMEOUT_MS;
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;
   assert(agent_save_config(&cfg) == 0);

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   assert(loaded.agent_count == 1);
   assert(loaded.agents[0].recommended_sampling == 1);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   else
      platform_unsetenv("HOME");
   if (old_aimee_home)
   {
      platform_setenv("AIMEE_HOME", old_aimee_home);
      free(old_aimee_home);
   }
   else
      platform_unsetenv("AIMEE_HOME");
   if (old_no_cache)
   {
      platform_setenv("AIMEE_NO_CACHE", old_no_cache);
      free(old_no_cache);
   }
   else
      platform_unsetenv("AIMEE_NO_CACHE");

   printf("agent_config_recommended_sampling_roundtrip OK\n");
}

static void test_parse_response_openai_rejects_invalid_tool_arguments(void)
{
   const char *json = "{"
                      "  \"choices\": [{"
                      "    \"finish_reason\": \"tool_calls\","
                      "    \"message\": {"
                      "      \"role\": \"assistant\","
                      "      \"content\": null,"
                      "      \"tool_calls\": [{"
                      "        \"id\": \"call_badargs\","
                      "        \"type\": \"function\","
                      "        \"function\": {"
                      "          \"name\": \"bash\","
                      "          \"arguments\": \"`\""
                      "        }"
                      "      }]"
                      "    }"
                      "  }]"
                      "}";

   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   aimee_response_t out;
   assert(agent_ir_decode_json_response_bound(root, 0, -1, NULL, NULL, &out) != 0);
   assert(aimee_ir_response_has_tool_use(&out) == 0 && aimee_ir_response_tool_count(&out) == 0);
   assert(out.n_content == 0 && out.content == NULL);

   aimee_response_free(&out);
   cJSON_Delete(root);
   printf("parse_response_openai_rejects_invalid_tool_arguments OK\n");
}

static void test_parse_response_captures_provider_model(void)
{
   /* The provider-reported model (response "model" field, often a more specific
    * dated version) is captured for billing precedence. */
   const char *oai = "{\"model\":\"gpt-4o-2024-11-20\",\"usage\":{\"prompt_tokens\":1,"
                     "\"completion_tokens\":1},\"choices\":[{\"message\":{\"content\":\"hi\"}}]}";
   cJSON *root = cJSON_Parse(oai);
   assert(root != NULL);
   aimee_response_t p;
   agent_ir_decode_json_response_bound(root, 0, -1, NULL, NULL, &p);
   assert(strcmp(p.model, "gpt-4o-2024-11-20") == 0);
   aimee_response_free(&p);
   cJSON_Delete(root);

   const char *ant = "{\"model\":\"claude-3-5-sonnet-20241022\",\"content\":[{\"type\":\"text\","
                     "\"text\":\"hi\"}],\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}";
   root = cJSON_Parse(ant);
   assert(root != NULL);
   agent_ir_decode_json_response_bound(root, 1, -1, NULL, NULL, &p);
   assert(strcmp(p.model, "claude-3-5-sonnet-20241022") == 0);
   aimee_response_free(&p);
   cJSON_Delete(root);
   printf("parse_response_captures_provider_model OK\n");
}

/* Regression: an Anthropic response can carry BOTH text and a tool_use in the same
 * turn (prose, then a tool call). The legacy parser used to drop the text whenever
 * stop_reason was tool_use -- a real bug the canonical IR did not have, which the
 * shadow flagged as ir_resp mismatches on live traffic. The text must survive
 * alongside the tool call. */
static void test_parse_response_keeps_text_with_tool_use(void)
{
   const char *ant =
       "{\"model\":\"m\",\"stop_reason\":\"tool_use\",\"content\":["
       "{\"type\":\"text\",\"text\":\"Let me check that.\"},"
       "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"grep\",\"input\":{\"q\":\"x\"}}]}";
   cJSON *root = cJSON_Parse(ant);
   assert(root != NULL);
   aimee_response_t p;
   memset(&p, 0, sizeof(p));
   agent_ir_decode_json_response_bound(root, 1, -1, NULL, NULL, &p);
   assert(aimee_ir_response_has_tool_use(&p) == 1);
   assert(aimee_ir_response_tool_count(&p) == 1 &&
          strcmp(aimee_ir_response_tool(&p, 0)->tool_name, "grep") == 0);
   assert_ir_text(&p, "Let me check that."); /* text NOT dropped */
   aimee_response_free(&p);
   cJSON_Delete(root);
   printf("parse_response_keeps_text_with_tool_use OK\n");
}

static void test_parse_response_openai_mistral_content_array(void)
{
   const char *json = "{"
                      "  \"choices\": [{"
                      "    \"finish_reason\": \"stop\","
                      "    \"message\": {"
                      "      \"role\": \"assistant\","
                      "      \"content\": ["
                      "        {\"type\": \"thinking\", \"thinking\": [{\"type\": \"text\", "
                      "\"text\": \"ignore me\"}]},"
                      "        {\"type\": \"text\", \"text\": \"CMAKE_DEP: schema_data_header\"}"
                      "      ]"
                      "    }"
                      "  }]"
                      "}";

   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   aimee_response_t out;
   agent_ir_decode_json_response_bound(root, 0, -1, NULL, NULL, &out);

   assert(aimee_ir_response_has_tool_use(&out) == 0);
   assert_ir_text(&out, "CMAKE_DEP: schema_data_header");

   aimee_response_free(&out);
   cJSON_Delete(root);
   printf("parse_response_openai_mistral_content_array OK\n");
}

static void test_parse_response_openai_stray_think_close(void)
{
   const char *json = "{"
                      "  \"choices\": [{"
                      "    \"finish_reason\": \"stop\","
                      "    \"message\": {"
                      "      \"role\": \"assistant\","
                      "      \"content\": \"</think>\\n\\nFinal answer\""
                      "    }"
                      "  }]"
                      "}";

   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   aimee_response_t out;
   agent_ir_decode_json_response_bound(root, 0, -1, NULL, NULL, &out);

   assert(aimee_ir_response_has_tool_use(&out) == 0);
   assert_ir_text(&out, "Final answer");

   aimee_response_free(&out);
   cJSON_Delete(root);
   printf("parse_response_openai_stray_think_close OK\n");
}

static void test_parse_response_openai_strips_thinking_block(void)
{
   const char *json = "{"
                      "  \"choices\": [{"
                      "    \"finish_reason\": \"stop\","
                      "    \"message\": {"
                      "      \"role\": \"assistant\","
                      "      \"content\": \"<think>hidden reasoning</think>LOCAL254_OK\""
                      "    }"
                      "  }],"
                      "  \"usage\": {\"prompt_tokens\": 3, \"completion_tokens\": 4}"
                      "}";

   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   aimee_response_t out;
   agent_ir_decode_json_response_bound(root, 0, -1, NULL, NULL, &out);

   assert(aimee_ir_response_has_tool_use(&out) == 0);
   assert_ir_text(&out, "LOCAL254_OK");
   assert(out.usage_in == 3);
   assert(out.usage_out == 4);

   aimee_response_free(&out);
   cJSON_Delete(root);
   printf("parse_response_openai_strips_thinking_block OK\n");
}

static void test_parse_response_openai_strips_thinking_process_scaffold(void)
{
   const char *json = "{"
                      "  \"choices\": [{"
                      "    \"finish_reason\": \"stop\","
                      "    \"message\": {"
                      "      \"role\": \"assistant\","
                      "      \"content\": \"*Thinking Process:*\\n\\n1. Inspect prompt.\\n\\n"
                      "6. **Final Output Generation:**\\n    *   \\\"Bundle is insufficient.\\\"\""
                      "    }"
                      "  }]"
                      "}";

   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   aimee_response_t out;
   agent_ir_decode_json_response_bound(root, 0, -1, NULL, NULL, &out);

   assert(aimee_ir_response_has_tool_use(&out) == 0);
   assert_ir_text(&out, "Bundle is insufficient.");

   aimee_response_free(&out);
   cJSON_Delete(root);
   printf("parse_response_openai_strips_thinking_process_scaffold OK\n");
}

static void test_parse_response_openai_strips_self_correction_scaffold(void)
{
   const char *json = "{"
                      "  \"choices\": [{"
                      "    \"finish_reason\": \"stop\","
                      "    \"message\": {"
                      "      \"role\": \"assistant\","
                      "      \"content\": \"*Self-Correction during thought process:*\\n"
                      "I should follow the exact prompt.\\n\\n"
                      "Final Answer:\\nlocal delegate ok\""
                      "    }"
                      "  }]"
                      "}";

   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   aimee_response_t out;
   agent_ir_decode_json_response_bound(root, 0, -1, NULL, NULL, &out);

   assert(aimee_ir_response_has_tool_use(&out) == 0);
   assert_ir_text(&out, "local delegate ok");

   aimee_response_free(&out);
   cJSON_Delete(root);
   printf("parse_response_openai_strips_self_correction_scaffold OK\n");
}

static void test_parse_response_openai_discards_private_scaffold_without_final(void)
{
   const char *json = "{"
                      "  \"choices\": [{"
                      "    \"finish_reason\": \"stop\","
                      "    \"message\": {"
                      "      \"role\": \"assistant\","
                      "      \"content\": \"Thought Process:\\n"
                      "I should not expose this internal reasoning.\""
                      "    }"
                      "  }]"
                      "}";

   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   aimee_response_t out;
   agent_ir_decode_json_response_bound(root, 0, -1, NULL, NULL, &out);

   assert(aimee_ir_response_has_tool_use(&out) == 0);
   /* No visible final text; the private scaffold remains separate. */
   assert_ir_text(&out, "");

   aimee_response_free(&out);
   cJSON_Delete(root);
   printf("parse_response_openai_discards_private_scaffold_without_final OK\n");
}

/* ----------------------------------------------------------------
 * OpenAI-compatible request shaping
 * ---------------------------------------------------------------- */

static cJSON *make_one_dummy_tool(void)
{
   cJSON *tools = cJSON_CreateArray();
   cJSON *tool = cJSON_CreateObject();
   cJSON *fn = cJSON_CreateObject();
   cJSON *params = cJSON_CreateObject();
   cJSON_AddStringToObject(tool, "type", "function");
   cJSON_AddStringToObject(fn, "name", "noop");
   cJSON_AddStringToObject(fn, "description", "No operation");
   cJSON_AddStringToObject(params, "type", "object");
   cJSON_AddItemToObject(fn, "parameters", params);
   cJSON_AddItemToObject(tool, "function", fn);
   cJSON_AddItemToArray(tools, tool);
   return tools;
}

static cJSON *make_one_user_message(void)
{
   cJSON *messages = cJSON_CreateArray();
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", "hello");
   cJSON_AddItemToArray(messages, user);
   return messages;
}

static void test_openai_request_strips_private_message_fields(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "mistral");
   snprintf(agent.model, sizeof(agent.model), "mistral-vibe-cli-latest");

   cJSON *messages = make_one_user_message();
   cJSON *user = cJSON_GetArrayItem(messages, 0);
   cJSON_AddBoolToObject(user, "_compaction_boundary", 1);

   cJSON *req = build_chat_profile(&agent, messages, NULL, 32, 0.0);
   assert(req != NULL);

   cJSON *out_messages = cJSON_GetObjectItem(req, "messages");
   assert(cJSON_IsArray(out_messages));
   cJSON *out_user = cJSON_GetArrayItem(out_messages, 0);
   assert(cJSON_GetObjectItem(out_user, "_compaction_boundary") == NULL);
   assert(cJSON_GetObjectItem(user, "_compaction_boundary") != NULL);

   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("openai_request_strips_private_message_fields OK\n");
}

static void test_openai_request_llama_compat_options(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "llama-eval");
   snprintf(agent.name, sizeof(agent.name), "llama-eval");
   snprintf(agent.model, sizeof(agent.model), "Qwen_Qwen3.6-35B-A3B-Q5_K_M.gguf");

   cJSON *messages = make_one_user_message();
   cJSON *tools = make_one_dummy_tool();
   cJSON *req = build_chat_profile(&agent, messages, tools, 32, 0.0);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "tools") != NULL);
   assert(cJSON_IsFalse(cJSON_GetObjectItem(req, "parallel_tool_calls")));
   assert(cJSON_GetObjectItem(req, "chat_template_kwargs") == NULL);
   cJSON *req_messages = cJSON_GetObjectItem(req, "messages");
   cJSON *req_user = cJSON_GetArrayItem(req_messages, 0);
   const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(req_user, "content"));
   assert(content != NULL && strncmp(content, "/no_think\n", 10) == 0);

   cJSON_Delete(req);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
   printf("openai_request_llama_compat_options OK\n");
}

static void test_openai_request_omits_empty_tools(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "gpt-4o-mini");

   cJSON *messages = make_one_user_message();
   cJSON *tools = cJSON_CreateArray();
   cJSON *req = build_chat_profile(&agent, messages, tools, 32, 0.0);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "tools") == NULL);
   assert(cJSON_GetObjectItem(req, "chat_template_kwargs") == NULL);

   cJSON_Delete(req);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
   printf("openai_request_omits_empty_tools OK\n");
}

static void test_evidence_review_requires_initial_tool_choice(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   snprintf(agent.model, sizeof(agent.model), "gpt-test");
   agent.require_initial_tool_call = 1;
   cJSON *messages = make_one_user_message();
   cJSON *tools = make_one_dummy_tool();

   cJSON *openai = build_chat_profile(&agent, messages, tools, 32, 0.0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(openai, "tool_choice")), "required") ==
          0);
   cJSON_Delete(openai);

   snprintf(agent.provider, sizeof(agent.provider), "minimax");
   snprintf(agent.model, sizeof(agent.model), "MiniMax-M3");
   cJSON *minimax = build_chat_profile(&agent, messages, tools, 32, 0.0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(minimax, "tool_choice")), "required") ==
          0);
   cJSON_Delete(minimax);

   snprintf(agent.provider, sizeof(agent.provider), "mistral");
   snprintf(agent.model, sizeof(agent.model), "mistral-large");
   cJSON *mistral = build_chat_profile(&agent, messages, tools, 32, 0.0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(mistral, "tool_choice")), "required") ==
          0);
   cJSON_Delete(mistral);

   cJSON *responses = build_responses_profile(&agent, messages, tools, "review");
   cJSON *responses_choice = cJSON_GetObjectItem(responses, "tool_choice");
   assert(cJSON_IsObject(responses_choice));
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(responses_choice, "type")), "function") ==
          0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(responses_choice, "name")), "noop") == 0);
   cJSON_Delete(responses);

   cJSON *response_tools = cJSON_CreateArray();
   cJSON *read_file = cJSON_CreateObject();
   cJSON_AddStringToObject(read_file, "type", "function");
   cJSON_AddStringToObject(read_file, "name", "read_file");
   cJSON_AddItemToArray(response_tools, read_file);
   cJSON *code_search = cJSON_CreateObject();
   cJSON_AddStringToObject(code_search, "type", "function");
   cJSON_AddStringToObject(code_search, "name", "code_search");
   cJSON_AddItemToArray(response_tools, code_search);
   responses = build_responses_profile(&agent, messages, response_tools, "review");
   responses_choice = cJSON_GetObjectItem(responses, "tool_choice");
   assert(cJSON_IsObject(responses_choice));
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(responses_choice, "name")),
                 "code_search") == 0);
   cJSON_Delete(responses);
   cJSON_Delete(response_tools);

   cJSON *malformed_tools = cJSON_CreateArray();
   cJSON_AddItemToArray(malformed_tools, cJSON_CreateObject());
   responses = build_responses_profile(&agent, messages, malformed_tools, "review");
   assert(cJSON_GetObjectItem(responses, "tool_choice") == NULL);
   cJSON_Delete(responses);
   cJSON_Delete(malformed_tools);

   cJSON *anthropic_tools = cJSON_Parse("[{\"name\":\"noop\",\"description\":\"No operation\","
                                        "\"input_schema\":{\"type\":\"object\"}}]");
   cJSON *anthropic = build_anthropic_profile(&agent, messages, anthropic_tools, "review", 32, 0.0);
   assert(anthropic);
   cJSON_Delete(anthropic_tools);
   cJSON *choice = cJSON_GetObjectItem(anthropic, "tool_choice");
   assert(cJSON_IsObject(choice));
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(choice, "type")), "any") == 0);
   cJSON_Delete(anthropic);

   cJSON_Delete(messages);
   cJSON_Delete(tools);
   assert(agent_require_initial_tool_choice(1, 0, 1) == 1);
   assert(agent_require_initial_tool_choice(1, 1, 1) == 0);
   assert(agent_require_initial_tool_choice(1, 0, 0) == 0);
   assert(agent_require_initial_tool_choice(0, 0, 1) == 0);
   printf("evidence_review_requires_initial_tool_choice OK\n");
}

static void test_provider_network_error_mentions_local_http_init(void)
{
   const char *msg = provider_error_message(PROVIDER_ERR_NETWORK);
   assert(strstr(msg, "local HTTP client") != NULL);
   assert(strstr(msg, "HTTP/SSL") != NULL);

   printf("provider_network_error_mentions_local_http_init OK\n");
}

/* ----------------------------------------------------------------
 * driver capabilities
 * ---------------------------------------------------------------- */

static void test_minimax_driver_has_own_caps(void)
{
   delegate_drivers_init();
   const delegate_driver_t *openai = delegate_driver_get("openai");
   const delegate_driver_t *minimax = delegate_driver_get("minimax");
   assert(openai != NULL);
   assert(minimax != NULL);
   assert(minimax != openai);

   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.model, sizeof(ag.model), "MiniMax-M2.7");
   driver_caps_t caps;
   minimax->get_caps(&ag, &caps);
   assert(caps.capability_flags & DRIVER_CAP_TOOL_CALLS);
   assert(caps.context_limit == 200000);

   printf("minimax_driver_has_own_caps OK\n");
}

/* Regression for the kb-down hang: the TCP-connect phase must be capped below
 * the overall request budget so an unreachable kb fast-fails instead of blocking
 * for the full 60s action timeout. */
static void test_connect_timeout_caps_below_request_budget(void)
{
   /* A short budget is used verbatim for connect. */
   assert(agent_http_effective_connect_timeout_ms(1000) == 1000);
   assert(agent_http_effective_connect_timeout_ms(AGENT_HTTP_CONNECT_TIMEOUT_MS) ==
          AGENT_HTTP_CONNECT_TIMEOUT_MS);
   /* A long budget (e.g. the 60s kb action timeout) is capped. */
   assert(agent_http_effective_connect_timeout_ms(60000) == AGENT_HTTP_CONNECT_TIMEOUT_MS);
   /* Non-positive budget -> capped default, never unbounded. */
   assert(agent_http_effective_connect_timeout_ms(0) == AGENT_HTTP_CONNECT_TIMEOUT_MS);
   assert(agent_http_effective_connect_timeout_ms(-1) == AGENT_HTTP_CONNECT_TIMEOUT_MS);
   printf("connect_timeout_caps_below_request_budget OK\n");
}

static int delegates_available_for_policy(void)
{
   return 1;
}

static void test_canonical_delegate_policy_before_native_egress(void)
{
   const char *tool_wires[] = {
       "[{\"type\":\"function\",\"function\":{\"name\":\"Read\",\"parameters\":{\"type\":"
       "\"object\"}}},"
       "{\"type\":\"function\",\"function\":{\"name\":\"Task\",\"parameters\":{\"type\":\"object\"}"
       "}}]",
       "[{\"name\":\"Read\",\"input_schema\":{\"type\":\"object\"}},"
       "{\"name\":\"Task\",\"input_schema\":{\"type\":\"object\"}}]",
       "[{\"type\":\"function\",\"name\":\"Read\",\"parameters\":{\"type\":\"object\"}},"
       "{\"type\":\"function\",\"name\":\"Task\",\"parameters\":{\"type\":\"object\"}}]"};
   const aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_ANTHROPIC,
                                 AIMEE_WIRE_RESPONSES};
   cJSON *messages = cJSON_Parse(
       "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"inspect the project\"}]}]");
   assert(messages);
   gateway_policy_set_delegates_available_provider(delegates_available_for_policy);
   for (size_t w = 0; w < sizeof wires / sizeof *wires; w++)
   {
      cJSON *tools = cJSON_Parse(tool_wires[w]);
      assert(tools);
      aimee_request_t dispatch = {0};
      for (int delegate = 0; delegate < 2; delegate++)
      {
         for (int fallback = 0; fallback < 2; fallback++)
         {
            agent_t agent = {0};
            snprintf(agent.model, sizeof agent.model, "%s", fallback ? "fallback" : "selected");
            agent.require_initial_tool_call = 1;
            cJSON *native = fixture_build_request(&agent, messages, tools, "host", 128, 0, wires[w],
                                                  delegate, &dispatch);
            assert(native);
            assert(!strcmp(cJSON_GetObjectItemCaseSensitive(native, "model")->valuestring,
                           agent.model));
            cJSON *catalog = cJSON_GetObjectItemCaseSensitive(native, "tools");
            assert(cJSON_GetArraySize(catalog) == (delegate ? 1 : 2));
            assert(dispatch.n_tools == (delegate ? 1 : 2) && !dispatch.n_messages &&
                   !dispatch.messages && !dispatch.system);
            const aimee_tool_t *read = aimee_ir_find_function(&dispatch, NULL, "Read");
            assert(read && cJSON_IsObject(read->schema));
            assert(!aimee_ir_find_function(&dispatch, "foreign", "Read"));
            assert(!!aimee_ir_find_function(&dispatch, NULL, "Task") == !delegate);
            assert(cJSON_GetObjectItemCaseSensitive(native, "tool_choice"));
            cJSON_Delete(native);
         }
      }
      assert(cJSON_GetArraySize(tools) == 2); /* Shared source is never modified. */
      /* Removing the last tool also clears the forced choice at real egress. */
      cJSON_DeleteItemFromArray(tools, 0);
      agent_t agent = {0};
      snprintf(agent.model, sizeof agent.model, "%s", "selected");
      agent.require_initial_tool_call = 1;
      cJSON *native =
          fixture_build_request(&agent, messages, tools, "host", 128, 0, wires[w], 1, &dispatch);
      assert(native && !cJSON_GetObjectItemCaseSensitive(native, "tools"));
      assert(!cJSON_GetObjectItemCaseSensitive(native, "tool_choice"));
      assert(!cJSON_GetObjectItemCaseSensitive(native, "parallel_tool_calls"));
      assert(!dispatch.n_tools && !aimee_ir_find_function(&dispatch, NULL, "Task"));
      aimee_request_free(&dispatch);
      cJSON_Delete(native);
      cJSON_Delete(tools);
   }
   /* An oversized catalog must be rejected before governance can remove all
    * its entries and accidentally turn an unadmitted request into valid egress. */
   for (size_t w = 0; w < sizeof wires / sizeof *wires; w++)
   {
      cJSON *examples = cJSON_Parse(tool_wires[w]);
      cJSON *task = cJSON_GetArrayItem(examples, 1);
      cJSON *oversized = cJSON_CreateArray();
      assert(examples && task && oversized);
      for (int i = 0; i <= AIMEE_IR_MAX_BLOCKS; i++)
         assert(cJSON_AddItemToArray(oversized, cJSON_Duplicate(task, 1)));
      agent_t agent = {0};
      snprintf(agent.model, sizeof agent.model, "selected");
      aimee_request_t owned = {0};
      assert(fixture_prepare_request(&agent, messages, oversized, "host", 128, 0, wires[w], 1,
                                     &owned) < 0);
      assert(!owned.model && !owned.n_tools && !owned.messages);
      assert(cJSON_GetArraySize(oversized) == AIMEE_IR_MAX_BLOCKS + 1);
      aimee_request_free(&owned);
      cJSON_Delete(oversized);
      cJSON_Delete(examples);
   }
   gateway_policy_set_delegates_available_provider(NULL);
   cJSON_Delete(messages);
   puts("canonical delegate policy before all native outputs/fallback models OK");
}

static void test_owned_canonical_request_preparation(void)
{
   const aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_ANTHROPIC,
                                 AIMEE_WIRE_RESPONSES};
   const char *catalogs[] = {
       "[{\"type\":\"function\",\"function\":{\"name\":\"Read\",\"parameters\":{\"type\":"
       "\"object\"}}}]",
       "[{\"name\":\"Read\",\"input_schema\":{\"type\":\"object\"}}]",
       "[{\"type\":\"function\",\"name\":\"Read\",\"parameters\":{\"type\":\"object\"}}]"};
   aimee_request_t owned = {0};
   for (size_t w = 0; w < sizeof wires / sizeof *wires; w++)
   {
      agent_t agent = {0};
      snprintf(agent.model, sizeof agent.model, "host-model-%zu", w);
      agent.require_initial_tool_call = 1;
      cJSON *messages = cJSON_Parse(
          "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"owned context\"}]},"
          "{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":\"original-"
          "call\","
          "\"name\":\"Read\",\"input\":{\"exact\":0.123456789012345678901234567890}}]}]");
      cJSON *tools = cJSON_Parse(catalogs[w]);
      assert(messages && tools);
      int rc = fixture_prepare_request(&agent, messages, tools, "host system", 321, 0.25, wires[w],
                                       0, &owned);
      cJSON_Delete(messages);
      cJSON_Delete(tools);
      assert(!rc && !aimee_ir_request_admit(&owned));
      assert(!strcmp(owned.model, agent.model) && owned.frontend == wires[w]);
      assert(owned.n_system == 1 && !strcmp(owned.system[0].text, "host system"));
      assert(owned.n_messages == 2 && owned.n_tools == 1);
      assert(!strcmp(owned.messages[0].blocks[0].text, "owned context"));
      const aimee_block_t *call = &owned.messages[1].blocks[0];
      assert(!strcmp(call->tool_id, "original-call") && !strcmp(call->tool_name, "Read"));
      char *input = cJSON_PrintUnformatted(call->tool_input);
      assert(input && !strcmp(input, "{\"exact\":0.123456789012345678901234567890}"));
      free(input);
      assert(aimee_ir_find_function(&owned, NULL, "Read"));
      assert(owned.tool_choice != AIMEE_TOOL_CHOICE_UNSET && owned.mutated);
      /* The Chat profile may carry its host-owned extension sidecar; it never
       * receives SDK prompt/catalog bytes or whole-request native replay. */
      assert(!owned.raw_bytes && !owned.raw_length);
      assert(!cJSON_GetObjectItemCaseSensitive(owned.raw, "messages"));
      assert(!cJSON_GetObjectItemCaseSensitive(owned.raw, "tools"));
      if (wires[w] != AIMEE_WIRE_RESPONSES)
         assert(owned.has_max_tokens && owned.max_tokens == 321);
      /* Failed replacement cannot erase the last admitted context/catalog. */
      char *model = owned.model;
      aimee_message_t *history = owned.messages;
      cJSON *invalid = cJSON_Parse("[{\"role\":\"user\",\"blocks\":[{\"type\":\"unknown\"}]}]");
      rc = fixture_prepare_request(&agent, invalid, NULL, "replacement", 1, 0, wires[w], 0, &owned);
      assert(rc < 0 && owned.model == model && owned.messages == history);
      cJSON_Delete(invalid);
   }
   aimee_request_free(&owned);
   puts("owned canonical host context/catalog survives source release and failed replacement OK");
}

static void test_canonical_request_profiles(void)
{
   const char *providers[] = {"ollama", "openrouter", "minimax", "mistral", "openai"};
   const char *models[] = {"qwen3.gguf", "selected", "MiniMax-M2.7", "mistral-vibe-cli", "gpt-4o"};
   cJSON *history =
       cJSON_Parse("[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"inspect\"}]}]");
   assert(history);
   for (size_t i = 0; i < sizeof providers / sizeof *providers; i++)
   {
      agent_t agent = {0};
      snprintf(agent.provider, sizeof agent.provider, "%s", providers[i]);
      snprintf(agent.model, sizeof agent.model, "%s", models[i]);
      agent.recommended_sampling = 1;
      cJSON *actual = fixture_build_request(&agent, history, NULL, NULL, 128, 0.5,
                                            AIMEE_WIRE_OPENAI_CHAT, 0, NULL);
      assert(actual);
      assert(!strcmp(cJSON_GetObjectItemCaseSensitive(actual, "model")->valuestring, models[i]));
      assert(cJSON_GetObjectItemCaseSensitive(actual, "max_tokens")->valueint == 128);
      const cJSON *message =
          cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(actual, "messages"), 0);
      assert(!strcmp(cJSON_GetObjectItemCaseSensitive(message, "content")->valuestring,
                     i == 0 ? "/no_think\ninspect" : "inspect"));
      cJSON_Delete(actual);
   }
   agent_t qwen = {0};
   snprintf(qwen.provider, sizeof qwen.provider, "%s", "ollama");
   snprintf(qwen.model, sizeof qwen.model, "%s", "qwen.gguf");
   const char *shaping_cases[] = {
       "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\" \"},"
       "{\"type\":\"text\",\"text\":\"/no_think\\ninspect\"}]}]",
       "[{\"role\":\"user\",\"blocks\":[]}]"};
   const char *expected[] = {" /no_think\ninspect", "/no_think\n"};
   for (size_t i = 0; i < sizeof shaping_cases / sizeof *shaping_cases; i++)
   {
      cJSON *source = cJSON_Parse(shaping_cases[i]);
      cJSON *before = cJSON_Duplicate(source, 1);
      cJSON *actual = fixture_build_request(&qwen, source, NULL, NULL, 128, 0.5,
                                            AIMEE_WIRE_OPENAI_CHAT, 0, NULL);
      assert(actual && cJSON_Compare(source, before, 1));
      const cJSON *message =
          cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(actual, "messages"), 0);
      assert(
          !strcmp(cJSON_GetObjectItemCaseSensitive(message, "content")->valuestring, expected[i]));
      cJSON_Delete(actual);
      cJSON_Delete(before);
      cJSON_Delete(source);
   }
   /* Hidden text must neither suppress shaping of visible text nor acquire it. */
   for (int visibility = 0; visibility < 2; ++visibility)
   {
      aimee_block_t blocks[] = {{.type = AIMEE_BLK_TEXT, .text = "/no_think\nHIDDEN_SECRET"},
                                {.type = AIMEE_BLK_TEXT, .text = "inspect"}};
      aimee_ir_block_set_context(&blocks[0], AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                                 AIMEE_CTX_TRUST_UNVERIFIED,
                                 visibility ? AIMEE_CTX_SENS_RESTRICTED : AIMEE_CTX_SENS_INTERNAL,
                                 visibility, NULL, NULL, 0);
      aimee_message_t typed = {.role = "user", .blocks = blocks, .n_blocks = 2};
      cJSON *source = cJSON_CreateArray();
      assert(source && cJSON_AddItemToArray(source, aimee_ir_message_json(&typed)));
      cJSON *before = cJSON_Duplicate(source, 1);
      cJSON *actual = fixture_build_request(&qwen, source, NULL, NULL, 128, 0.5,
                                            AIMEE_WIRE_OPENAI_CHAT, 0, NULL);
      assert(actual && cJSON_Compare(source, before, 1));
      const cJSON *message =
          cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(actual, "messages"), 0);
      const char *content =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "content"));
      assert(content && !strcmp(content, "/no_think\ninspect"));
      cJSON_Delete(actual);
      cJSON_Delete(before);
      cJSON_Delete(source);
   }
   /* Shaping is idempotent and never edits durable canonical source content. */
   const cJSON *block = cJSON_GetArrayItem(
       cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(history, 0), "blocks"), 0);
   assert(!strcmp(cJSON_GetObjectItemCaseSensitive(block, "text")->valuestring, "inspect"));
   cJSON_Delete(history);
   puts("canonical request profile provider settings and source isolation OK");
}

static void test_canonical_responses_profile(void)
{
   agent_t agent = {0};
   snprintf(agent.model, sizeof agent.model, "%s", "selected");
   agent.require_initial_tool_call = 1;
   cJSON *history =
       cJSON_Parse("[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"inspect\"}]}]");
   cJSON *tools = cJSON_Parse(
       "[{\"type\":\"function\",\"name\":\"other\",\"parameters\":{\"type\":\"object\"}},"
       "{\"type\":\"function\",\"name\":\"read_file\",\"parameters\":{\"type\":\"object\"}},"
       "{\"type\":\"function\",\"name\":\"code_search\",\"parameters\":{\"type\":\"object\"}}]");
   assert(history && tools);
   for (int fallback = 0; fallback < 2; fallback++)
   {
      cJSON *actual = fixture_build_request(&agent, history, tools, NULL, 777, 0.5,
                                            AIMEE_WIRE_RESPONSES, 0, NULL);
      assert(actual && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(actual, "store")));
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(actual, "stream")));
      assert(!strcmp(cJSON_GetObjectItemCaseSensitive(actual, "instructions")->valuestring,
                     "You are an execution agent."));
      const cJSON *choice = cJSON_GetObjectItemCaseSensitive(actual, "tool_choice");
      assert(choice && !strcmp(cJSON_GetObjectItemCaseSensitive(choice, "name")->valuestring,
                               fallback ? "other" : "code_search"));
      assert(!cJSON_GetObjectItemCaseSensitive(actual, "max_output_tokens"));
      assert(!cJSON_GetObjectItemCaseSensitive(actual, "temperature"));
      cJSON_Delete(actual);
      cJSON_DeleteItemFromArray(tools, 2);
      cJSON_DeleteItemFromArray(tools, 1);
   }
   /* Malformed catalog ingress still refuses before any provider dispatch. */
   cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetArrayItem(tools, 0), "parameters");
   cJSON_AddStringToObject(cJSON_GetArrayItem(tools, 0), "parameters", "malformed");
   cJSON *actual = fixture_build_request(&agent, history, tools, "host", 777, 0.5,
                                         AIMEE_WIRE_RESPONSES, 0, NULL);
   assert(!actual);
   cJSON_Delete(history);
   cJSON_Delete(tools);
   puts("typed Responses host profile, forced evidence choice and catalog refusal OK");
}

static void test_typed_anthropic_system_cache_parity(void)
{
   const char *systems[] = {NULL,
                            "",
                            "stable",
                            "stable<aimee-context>volatile",
                            " <aimee-context>volatile",
                            "x<aimee-context>long volatile"};
   /* Independent wire goldens replace comparison to the retired second builder. */
   const char *cached[] = {
       NULL,
       NULL,
       "[{\"type\":\"text\",\"text\":\"stable\",\"cache_control\":{\"type\":\"ephemeral\"}}]",
       "[{\"type\":\"text\",\"text\":\"stable\",\"cache_control\":{\"type\":\"ephemeral\"}},{"
       "\"type\":\"text\",\"text\":\"<aimee-context>volatile\"}]",
       NULL,
       "[{\"type\":\"text\",\"text\":\"x\",\"cache_control\":{\"type\":\"ephemeral\"}},{\"type\":"
       "\"text\",\"text\":\"<aimee-context>long volatile\"}]"};
   for (size_t i = 0; i < sizeof systems / sizeof *systems; i++)
      for (int cache = 0; cache < 2; cache++)
         for (int floor = 0; floor <= 8; floor += 8)
         {
            cJSON *expected = cache && !floor && cached[i]  ? cJSON_Parse(cached[i])
                              : systems[i] && systems[i][0] ? cJSON_CreateString(systems[i])
                                                            : NULL;
            aimee_request_t ir = {.model = strdup("selected"),
                                  .frontend = AIMEE_WIRE_ANTHROPIC,
                                  .max_tokens = 128,
                                  .has_max_tokens = 1};
            assert(!anthropic_ir_set_host_system(&ir, systems[i], cache, floor));
            cJSON *actual = anthropic_backend_build(&ir);
            assert(actual);
            const cJSON *left = expected;
            const cJSON *right = cJSON_GetObjectItemCaseSensitive(actual, "system");
            assert((!left && !right) || cJSON_Compare(left, right, 1));
            if (left && right)
            {
               char *left_wire = cJSON_PrintUnformatted(left);
               char *right_wire = cJSON_PrintUnformatted(right);
               assert(left_wire && right_wire && !strcmp(left_wire, right_wire));
               free(left_wire);
               free(right_wire);
            }
            cJSON_Delete(actual);
            cJSON_Delete(expected);
            aimee_request_free(&ir);
         }
   aimee_request_t edited = {.model = strdup("selected"),
                             .frontend = AIMEE_WIRE_ANTHROPIC,
                             .max_tokens = 128,
                             .has_max_tokens = 1};
   assert(!anthropic_ir_set_host_system(&edited, "old<aimee-context>volatile", 1, 0));
   cJSON *entry = edited.raw;
   assert(anthropic_ir_set_host_system(&edited, "replacement", 0, 0) == -1 && edited.raw == entry);
   free(edited.system[0].text);
   edited.system[0].text = strdup("policy changed");
   cJSON *changed = anthropic_backend_build(&edited);
   assert(changed);
   const cJSON *blocks = cJSON_GetObjectItemCaseSensitive(changed, "system");
   assert(cJSON_IsArray(blocks) && cJSON_GetArraySize(blocks) == 2);
   assert(
       !strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(blocks, 0), "text")->valuestring,
               "policy changed"));
   assert(
       !strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(blocks, 1), "text")->valuestring,
               "<aimee-context>volatile"));
   cJSON_Delete(changed);
   aimee_request_free(&edited);
   puts("typed Anthropic system/cache scalar and block byte parity OK");
}

static void test_missing_response_output(void)
{
   assert(agent_http_get("http://127.0.0.1:9", NULL, NULL, 1) == -1);
   assert(agent_http_post_content_type("http://127.0.0.1:9", NULL, "application/json", "{}", NULL,
                                       1, NULL) == -1);
}

static void test_canonical_catalog_only_projection(void)
{
   cJSON *history =
       cJSON_Parse("[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"inspect\"}]}]");
   aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_RESPONSES, AIMEE_WIRE_ANTHROPIC};
   for (int w = 0; w < 3; ++w)
   {
      aimee_request_t catalog = {0}, expected = {0}, actual = {0};
      agent_t agent = {0};
      snprintf(agent.model, sizeof agent.model, "selected");
      assert(!agent_tools_catalog(wires[w], &catalog));
      assert(agent_tools_catalog_respond(&catalog) == 1);
      assert(!fixture_prepare_catalog(&agent, history, &catalog, "host", 128, 0.2, wires[w], 0,
                                      &expected));
      catalog.stream = 1;
      catalog.tool_choice = AIMEE_TOOL_CHOICE_NONE;
      catalog.has_temperature = 1;
      catalog.temperature = 0.9;
      catalog.has_max_tokens = 1;
      catalog.max_tokens = 1;
      catalog.output_format.kind = AIMEE_OUTPUT_JSON_OBJECT;
      catalog.has_parallel_tool_calls = 1;
      catalog.parallel_tool_calls = 0;
      assert(!aimee_ir_request_admit(&catalog));
      assert(!fixture_prepare_catalog(&agent, history, &catalog, "host", 128, 0.2, wires[w], 0,
                                      &actual));
      assert(actual.stream == expected.stream && actual.tool_choice == expected.tool_choice &&
             actual.output_format.kind == expected.output_format.kind);
      assert(actual.max_tokens == expected.max_tokens &&
             actual.temperature == expected.temperature);
      assert(actual.has_parallel_tool_calls == expected.has_parallel_tool_calls &&
             actual.parallel_tool_calls == expected.parallel_tool_calls);
      assert(actual.n_tools == expected.n_tools && actual.tools != catalog.tools);
      const aimee_tool_t *respond = aimee_ir_find_function(&actual, NULL, "respond");
      assert(respond);
      cJSON *wire =
          fixture_build_catalog(&agent, history, &catalog, "host", 128, 0.2, wires[w], 0, NULL);
      assert(wire && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(wire, "tools")) ==
                         catalog.n_tools);
      /* Typed respond is valid on Anthropic too; the retired wrapped-Chat append
       * was invalid input to that old profile, not a wire-preservation case. */
      cJSON_Delete(wire);
      aimee_request_free(&catalog);
      aimee_request_free(&expected);
      aimee_request_free(&actual);
   }
   cJSON_Delete(history);
   puts("tools-only profile clone rejects metadata contamination; all profiles render typed "
        "respond OK");
}

#include "support/offline_model_request_fixture.h"
int main(int argc, char **argv)
{
#ifndef _WIN32
   if (argc > 1)
      return offline_model_request_diagnostic(argc, argv);
#else
   (void)argc;
   (void)argv;
#endif
   test_canonical_catalog_only_projection();
   test_typed_anthropic_system_cache_parity();
   test_canonical_responses_profile();
   test_canonical_request_profiles();
   test_owned_canonical_request_preparation();
   test_missing_response_output();
   printf("test_agent_http: ");

   test_canonical_delegate_policy_before_native_egress();
   test_connect_timeout_caps_below_request_budget();
   test_build_request_openai_omits_empty_tools();
   test_openai_request_strips_private_message_fields();
   test_build_request_openai_qwen_profile();
   test_build_request_openai_qwen_no_tools_disables_thinking();
   test_build_request_openai_standard_tools();
   test_build_request_openrouter_routing_hint();
   test_build_request_openai_mistral_vibe_options();
   test_build_request_openai_minimax_options();
   test_model_sampling_lookup_qwen_stem();
   test_openai_recommended_sampling_applies_map();
   test_openai_recommended_sampling_ministral_temperature();
   test_openai_sampling_caller_temperature_wins();
   test_openai_sampling_opt_out_unchanged();
   test_openai_sampling_unknown_opt_in_unchanged();
   test_openai_provider_fixed_temperature_fallback();
   test_openai_required_temperature_overrides_caller();
   test_agent_config_recommended_sampling_roundtrip();
   test_parse_response_openai_rejects_invalid_tool_arguments();
   test_parse_response_captures_provider_model();
   test_parse_response_keeps_text_with_tool_use();
   test_parse_response_openai_mistral_content_array();
   test_parse_response_openai_stray_think_close();
   test_parse_response_openai_strips_thinking_block();
   test_parse_response_openai_strips_thinking_process_scaffold();
   test_parse_response_openai_strips_self_correction_scaffold();
   test_parse_response_openai_discards_private_scaffold_without_final();

   test_openai_request_llama_compat_options();
   test_openai_request_omits_empty_tools();
   test_evidence_review_requires_initial_tool_choice();
   test_provider_network_error_mentions_local_http_init();
   test_minimax_driver_has_own_caps();

   printf("all tests passed\n");
   return 0;
}
