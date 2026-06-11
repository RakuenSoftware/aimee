/* test_agent_http.c: unit tests for Gemini prompt-cache lifecycle functions
 *
 * Tests cover:
 *   - gemini_prompt_cache_attach: request mutation (add cachedContent, remove systemInstruction)
 *   - gemini_prompt_cache_create: graceful failure with invalid/empty inputs
 *   - agent_build_request_gemini: request shape with and without a cache name
 *   - agent_parse_response_gemini: cache hit token tracking via cachedContentTokenCount
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_protocol.h"
#include "delegate_driver.h"
#include "delegate_xml_fallback.h"
#include "model_provider.h"
#include "model_sampling.h"
#include "platform_test_util.h"
#include "cJSON.h"

static cJSON *parse_json_or_die(const char *json)
{
   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);
   return root;
}

/* ----------------------------------------------------------------
 * gemini_prompt_cache_attach
 * ---------------------------------------------------------------- */

static void test_cache_attach_adds_field(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "systemInstruction", "be helpful");
   cJSON_AddStringToObject(req, "model", "gemini-1.5-pro");

   gemini_prompt_cache_attach(req, "cachedContents/abc123");

   /* cachedContent field should be present */
   cJSON *cc = cJSON_GetObjectItem(req, "cachedContent");
   assert(cc != NULL);
   assert(cJSON_IsString(cc));
   assert(strcmp(cc->valuestring, "cachedContents/abc123") == 0);

   /* systemInstruction must be removed (it lives in the cache) */
   assert(cJSON_GetObjectItem(req, "systemInstruction") == NULL);

   cJSON_Delete(req);
   printf("cache_attach_adds_field OK\n");
}

static void test_cache_attach_noop_on_empty_name(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "systemInstruction", "be helpful");

   gemini_prompt_cache_attach(req, "");

   /* No cachedContent should be added */
   assert(cJSON_GetObjectItem(req, "cachedContent") == NULL);

   /* systemInstruction should remain (cache name was empty) */
   cJSON *si = cJSON_GetObjectItem(req, "systemInstruction");
   assert(si != NULL);

   cJSON_Delete(req);
   printf("cache_attach_noop_on_empty_name OK\n");
}

static void test_cache_attach_noop_on_null(void)
{
   /* Must not crash */
   gemini_prompt_cache_attach(NULL, "cachedContents/abc");
   gemini_prompt_cache_attach(NULL, NULL);

   cJSON *req = cJSON_CreateObject();
   gemini_prompt_cache_attach(req, NULL);
   assert(cJSON_GetObjectItem(req, "cachedContent") == NULL);
   cJSON_Delete(req);

   printf("cache_attach_noop_on_null OK\n");
}

/* ----------------------------------------------------------------
 * gemini_prompt_cache_create — failure path (no network needed)
 * ---------------------------------------------------------------- */

static void test_cache_create_rejects_empty_inputs(void)
{
   char name[256];

   /* Empty model → -1, name stays empty */
   int rc = gemini_prompt_cache_create(NULL, "Bearer tok", "", "system prompt", 1000, name,
                                       sizeof(name));
   assert(rc == -1);
   assert(name[0] == '\0');

   /* Empty system prompt → -1 */
   rc = gemini_prompt_cache_create(NULL, "Bearer tok", "gemini-1.5-pro", "", 1000, name,
                                   sizeof(name));
   assert(rc == -1);
   assert(name[0] == '\0');

   printf("cache_create_rejects_empty_inputs OK\n");
}

/* ----------------------------------------------------------------
 * agent_build_request_gemini
 * ---------------------------------------------------------------- */

static void test_build_request_gemini_uncached(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.model, sizeof(agent.model), "gemini-1.5-pro");

   cJSON *messages = cJSON_CreateArray();
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", "hello");
   cJSON_AddItemToArray(messages, user);

   cJSON *req = agent_build_request_gemini(&agent, messages, NULL, "be helpful", 1024, 0.0, NULL);
   assert(req != NULL);

   /* systemInstruction must be present when no cache */
   cJSON *si = cJSON_GetObjectItem(req, "systemInstruction");
   assert(si != NULL);

   /* cachedContent must NOT be present */
   assert(cJSON_GetObjectItem(req, "cachedContent") == NULL);

   /* contents array must be present */
   cJSON *contents = cJSON_GetObjectItem(req, "contents");
   assert(contents != NULL);
   assert(cJSON_IsArray(contents));
   assert(cJSON_GetArraySize(contents) == 1);

   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("build_request_gemini_uncached OK\n");
}

static void test_build_request_gemini_cached(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.model, sizeof(agent.model), "gemini-1.5-pro");

   cJSON *messages = cJSON_CreateArray();
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", "hello");
   cJSON_AddItemToArray(messages, user);

   cJSON *req = agent_build_request_gemini(&agent, messages, NULL, "be helpful", 1024, 0.0,
                                           "cachedContents/xyz789");
   assert(req != NULL);

   /* systemInstruction must NOT be present (it lives in the cache) */
   assert(cJSON_GetObjectItem(req, "systemInstruction") == NULL);

   /* cachedContent must reference the provided cache name */
   cJSON *cc = cJSON_GetObjectItem(req, "cachedContent");
   assert(cc != NULL);
   assert(cJSON_IsString(cc));
   assert(strcmp(cc->valuestring, "cachedContents/xyz789") == 0);

   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("build_request_gemini_cached OK\n");
}

static void test_build_request_gemini_tool_result_conversion(void)
{
   /* Verify that role=tool messages are converted to Gemini functionResponse parts */
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.model, sizeof(agent.model), "gemini-1.5-pro");

   cJSON *messages = cJSON_CreateArray();

   /* User turn */
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", "what is 2+2?");
   cJSON_AddItemToArray(messages, user);

   /* Assistant tool call (stored in assistant-with-parts Gemini style) */
   cJSON *asst = cJSON_CreateObject();
   cJSON_AddStringToObject(asst, "role", "assistant");
   cJSON *asst_parts = cJSON_CreateArray();
   cJSON *fc_part = cJSON_CreateObject();
   cJSON *fc = cJSON_CreateObject();
   cJSON_AddStringToObject(fc, "name", "calculate");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "expression", "2+2");
   cJSON_AddItemToObject(fc, "args", args);
   cJSON_AddItemToObject(fc_part, "functionCall", fc);
   cJSON_AddItemToArray(asst_parts, fc_part);
   cJSON_AddItemToObject(asst, "parts", asst_parts);
   cJSON_AddItemToArray(messages, asst);

   /* Tool result (OpenAI format with tool_call_id = function name for Gemini) */
   cJSON *tool_result = cJSON_CreateObject();
   cJSON_AddStringToObject(tool_result, "role", "tool");
   cJSON_AddStringToObject(tool_result, "tool_call_id", "calculate");
   cJSON_AddStringToObject(tool_result, "content", "4");
   cJSON_AddItemToArray(messages, tool_result);

   cJSON *req = agent_build_request_gemini(&agent, messages, NULL, NULL, 0, 0.0, NULL);
   assert(req != NULL);

   cJSON *contents = cJSON_GetObjectItem(req, "contents");
   assert(contents != NULL);
   /* Should have 3 turns: user, model (function call), user (function response) */
   assert(cJSON_GetArraySize(contents) == 3);

   /* Last item should be role=user with functionResponse part */
   cJSON *last = cJSON_GetArrayItem(contents, 2);
   cJSON *last_role = cJSON_GetObjectItem(last, "role");
   assert(last_role != NULL);
   assert(strcmp(last_role->valuestring, "user") == 0);

   cJSON *last_parts = cJSON_GetObjectItem(last, "parts");
   assert(last_parts != NULL);
   assert(cJSON_GetArraySize(last_parts) == 1);

   cJSON *fr_part = cJSON_GetArrayItem(last_parts, 0);
   cJSON *func_resp = cJSON_GetObjectItem(fr_part, "functionResponse");
   assert(func_resp != NULL);

   cJSON *fn_name = cJSON_GetObjectItem(func_resp, "name");
   assert(fn_name != NULL);
   assert(strcmp(fn_name->valuestring, "calculate") == 0);

   cJSON_Delete(req);
   cJSON_Delete(messages);
   printf("build_request_gemini_tool_result_conversion OK\n");
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, 0.0);
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
   cJSON *req = agent_build_request_openai(&agent, messages, tools, 1024, 0.0);
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, 0.0);
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
   cJSON *req = agent_build_request_openai(&agent, messages, tools, 1024, 0.0);
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, 0.0);
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, 0.0);
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, 0.0);
   assert(req != NULL);

   cJSON *split = cJSON_GetObjectItem(req, "reasoning_split");
   assert(cJSON_IsTrue(split));

   cJSON_Delete(req);
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, -1);
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, -1);
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, 0.42);
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, -1);
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, -1);
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
   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 1024, -1);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "temperature")->valuedouble == 0.0);
   cJSON_Delete(req);
   cJSON_Delete(messages);

   provider->fixed_temperature = old_fixed_temperature;
   printf("openai_provider_fixed_temperature_fallback OK\n");
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

static void test_parse_response_openai_sanitizes_invalid_tool_arguments(void)
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

   parsed_response_t out;
   agent_parse_response_openai(root, &out);

   assert(out.is_tool_call == 1);
   assert(out.call_count == 1);
   assert(strcmp(out.calls[0].name, "bash") == 0);
   assert(strcmp(out.calls[0].arguments, "{}") == 0);

   cJSON *tool_calls = cJSON_GetObjectItem(out.assistant_message, "tool_calls");
   cJSON *tc = cJSON_GetArrayItem(tool_calls, 0);
   cJSON *fn = cJSON_GetObjectItem(tc, "function");
   cJSON *args = cJSON_GetObjectItem(fn, "arguments");
   assert(cJSON_IsString(args));
   assert(strcmp(args->valuestring, "{}") == 0);

   agent_free_parsed_response(&out);
   cJSON_Delete(root);
   printf("parse_response_openai_sanitizes_invalid_tool_arguments OK\n");
}

static void test_parse_response_captures_provider_model(void)
{
   /* The provider-reported model (response "model" field, often a more specific
    * dated version) is captured for billing precedence. */
   const char *oai = "{\"model\":\"gpt-4o-2024-11-20\",\"usage\":{\"prompt_tokens\":1,"
                     "\"completion_tokens\":1},\"choices\":[{\"message\":{\"content\":\"hi\"}}]}";
   cJSON *root = cJSON_Parse(oai);
   assert(root != NULL);
   parsed_response_t p;
   agent_parse_response_openai(root, &p);
   assert(strcmp(p.model, "gpt-4o-2024-11-20") == 0);
   agent_free_parsed_response(&p);
   cJSON_Delete(root);

   const char *ant = "{\"model\":\"claude-3-5-sonnet-20241022\",\"content\":[{\"type\":\"text\","
                     "\"text\":\"hi\"}],\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}";
   root = cJSON_Parse(ant);
   assert(root != NULL);
   agent_parse_response_anthropic(root, &p);
   assert(strcmp(p.model, "claude-3-5-sonnet-20241022") == 0);
   agent_free_parsed_response(&p);
   cJSON_Delete(root);
   printf("parse_response_captures_provider_model OK\n");
}

static void test_delegate_rescue_parses_mistral_bracket(void)
{
   parsed_response_t parsed;
   memset(&parsed, 0, sizeof(parsed));

   const char *text = "[TOOL_CALLS]read_file{\"path\":\"x\",\"meta\":{\"brace\":\"{ok}\"}}";
   assert(delegate_rescue_has_tool_calls_with_json(text, 1) == 1);
   assert(xml_parse_tool_calls(text, &parsed) == 1);
   assert(parsed.is_tool_call == 1);
   assert(parsed.call_count == 1);
   assert(strcmp(parsed.calls[0].name, "read_file") == 0);

   cJSON *args = parse_json_or_die(parsed.calls[0].arguments);
   cJSON *path = cJSON_GetObjectItemCaseSensitive(args, "path");
   cJSON *meta = cJSON_GetObjectItemCaseSensitive(args, "meta");
   cJSON *brace = cJSON_GetObjectItemCaseSensitive(meta, "brace");
   assert(cJSON_IsString(path) && strcmp(path->valuestring, "x") == 0);
   assert(cJSON_IsString(brace) && strcmp(brace->valuestring, "{ok}") == 0);
   cJSON_Delete(args);
   agent_free_parsed_response(&parsed);
   printf("delegate_rescue_parses_mistral_bracket OK\n");
}

static void test_delegate_rescue_parses_fenced_json(void)
{
   parsed_response_t parsed;
   memset(&parsed, 0, sizeof(parsed));

   const char *text = "```json\n{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}\n```";
   assert(delegate_rescue_has_tool_calls_with_json(text, 1) == 1);
   assert(xml_parse_tool_calls(text, &parsed) == 1);
   assert(parsed.is_tool_call == 1);
   assert(parsed.call_count == 1);
   assert(strcmp(parsed.calls[0].name, "bash") == 0);

   cJSON *args = parse_json_or_die(parsed.calls[0].arguments);
   cJSON *command = cJSON_GetObjectItemCaseSensitive(args, "command");
   assert(cJSON_IsString(command) && strcmp(command->valuestring, "ls") == 0);
   cJSON_Delete(args);
   agent_free_parsed_response(&parsed);
   printf("delegate_rescue_parses_fenced_json OK\n");
}

static void test_delegate_rescue_detects_invoke_markup(void)
{
   parsed_response_t parsed;
   memset(&parsed, 0, sizeof(parsed));

   const char *text = "<invoke name=\"bash\">\n"
                      "<parameter name=\"command\">echo invoke-ok</parameter>\n"
                      "</invoke>";
   assert(delegate_rescue_has_tool_calls_with_json(text, 1) == 1);
   assert(xml_parse_tool_calls(text, &parsed) == 1);
   assert(parsed.is_tool_call == 1);
   assert(parsed.call_count == 1);
   assert(strcmp(parsed.calls[0].name, "bash") == 0);
   assert(strstr(parsed.calls[0].arguments, "invoke-ok") != NULL);
   agent_free_parsed_response(&parsed);
   printf("delegate_rescue_detects_invoke_markup OK\n");
}

static void test_delegate_rescue_skips_malformed_invoke_markup(void)
{
   parsed_response_t parsed;
   memset(&parsed, 0, sizeof(parsed));

   const char *text = "<invoke>\n"
                      "<parameter name=\"command\">echo ignored</parameter>\n"
                      "</invoke>\n"
                      "<invoke name=\"bash\">\n"
                      "<parameter name=\"command\">echo parsed</parameter>\n"
                      "</invoke>";
   assert(delegate_rescue_has_tool_calls_with_json(text, 1) == 1);
   assert(xml_parse_tool_calls(text, &parsed) == 1);
   assert(parsed.is_tool_call == 1);
   assert(parsed.call_count == 1);
   assert(strcmp(parsed.calls[0].name, "bash") == 0);
   assert(strstr(parsed.calls[0].arguments, "parsed") != NULL);
   agent_free_parsed_response(&parsed);
   printf("delegate_rescue_skips_malformed_invoke_markup OK\n");
}

static void test_delegate_rescue_channel_balances_braces(void)
{
   parsed_response_t parsed;
   memset(&parsed, 0, sizeof(parsed));

   const char *text = "<|channel>call: bash {command: \"echo } ok\"}<tool_call|>";
   assert(delegate_rescue_has_tool_calls_with_json(text, 1) == 1);
   assert(xml_parse_tool_calls(text, &parsed) == 1);
   assert(parsed.is_tool_call == 1);
   assert(parsed.call_count == 1);
   assert(strcmp(parsed.calls[0].name, "bash") == 0);

   cJSON *args = parse_json_or_die(parsed.calls[0].arguments);
   cJSON *command = cJSON_GetObjectItemCaseSensitive(args, "command");
   assert(cJSON_IsString(command) && strcmp(command->valuestring, "echo } ok") == 0);
   cJSON_Delete(args);
   agent_free_parsed_response(&parsed);
   printf("delegate_rescue_channel_balances_braces OK\n");
}

static void test_delegate_rescue_strips_think_blocks(void)
{
   parsed_response_t parsed;
   memset(&parsed, 0, sizeof(parsed));

   const char *text =
       "<think>I should call a tool.</think>[TOOL_CALLS]bash{\"command\":\"echo ok\"}";
   assert(delegate_rescue_has_tool_calls_with_json(text, 1) == 1);
   assert(xml_parse_tool_calls(text, &parsed) == 1);
   assert(parsed.is_tool_call == 1);
   assert(parsed.call_count == 1);
   assert(strcmp(parsed.calls[0].name, "bash") == 0);
   assert(strstr(parsed.calls[0].arguments, "echo ok") != NULL);
   agent_free_parsed_response(&parsed);
   printf("delegate_rescue_strips_think_blocks OK\n");
}

static void test_delegate_rescue_rejects_unknown_json_tool(void)
{
   parsed_response_t parsed;
   memset(&parsed, 0, sizeof(parsed));

   const char *text = "{\"tool\":\"definitely_not_a_tool\",\"args\":{\"x\":1}}";
   assert(delegate_rescue_has_tool_calls_with_json(text, 1) == 0);
   assert(xml_parse_tool_calls(text, &parsed) == 0);
   assert(parsed.is_tool_call == 0);
   assert(parsed.call_count == 0);
   agent_free_parsed_response(&parsed);
   printf("delegate_rescue_rejects_unknown_json_tool OK\n");
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

   parsed_response_t out;
   agent_parse_response_openai(root, &out);

   assert(out.is_tool_call == 0);
   assert(out.content != NULL);
   assert(strcmp(out.content, "CMAKE_DEP: schema_data_header") == 0);

   agent_free_parsed_response(&out);
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

   parsed_response_t out;
   agent_parse_response_openai(root, &out);

   assert(out.is_tool_call == 0);
   assert(out.content != NULL);
   assert(strcmp(out.content, "Final answer") == 0);

   agent_free_parsed_response(&out);
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

   parsed_response_t out;
   agent_parse_response_openai(root, &out);

   assert(out.is_tool_call == 0);
   assert(out.content != NULL);
   assert(strcmp(out.content, "LOCAL254_OK") == 0);
   assert(out.prompt_tokens == 3);
   assert(out.completion_tokens == 4);

   agent_free_parsed_response(&out);
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

   parsed_response_t out;
   agent_parse_response_openai(root, &out);

   assert(out.is_tool_call == 0);
   assert(out.content != NULL);
   assert(strcmp(out.content, "Bundle is insufficient.") == 0);

   agent_free_parsed_response(&out);
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

   parsed_response_t out;
   agent_parse_response_openai(root, &out);

   assert(out.is_tool_call == 0);
   assert(out.content != NULL);
   assert(strcmp(out.content, "local delegate ok") == 0);

   agent_free_parsed_response(&out);
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

   parsed_response_t out;
   agent_parse_response_openai(root, &out);

   assert(out.is_tool_call == 0);
   assert(out.content != NULL);
   assert(strcmp(out.content, "") == 0);

   agent_free_parsed_response(&out);
   cJSON_Delete(root);
   printf("parse_response_openai_discards_private_scaffold_without_final OK\n");
}

/* ----------------------------------------------------------------
 * agent_parse_response_gemini — cache hit token tracking
 * ---------------------------------------------------------------- */

static void test_parse_response_gemini_cache_hit(void)
{
   /* Simulate a Gemini response that includes cachedContentTokenCount */
   const char *json = "{"
                      "  \"candidates\": [{"
                      "    \"content\": {"
                      "      \"role\": \"model\","
                      "      \"parts\": [{\"text\": \"The answer is 4.\"}]"
                      "    },"
                      "    \"finishReason\": \"STOP\""
                      "  }],"
                      "  \"usageMetadata\": {"
                      "    \"promptTokenCount\": 500,"
                      "    \"candidatesTokenCount\": 10,"
                      "    \"cachedContentTokenCount\": 450"
                      "  }"
                      "}";

   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   parsed_response_t out;
   agent_parse_response_gemini(root, &out);

   assert(out.prompt_tokens == 500);
   assert(out.completion_tokens == 10);
   /* cachedContentTokenCount should appear as cache_read_tokens */
   assert(out.cache_read_tokens == 450);
   assert(out.cache_write_tokens == 0);
   assert(out.content != NULL);
   assert(strstr(out.content, "answer is 4") != NULL);
   assert(out.is_tool_call == 0);

   agent_free_parsed_response(&out);
   cJSON_Delete(root);
   printf("parse_response_gemini_cache_hit OK\n");
}

static void test_parse_response_gemini_no_cache(void)
{
   /* Normal response without caching */
   const char *json = "{"
                      "  \"candidates\": [{"
                      "    \"content\": {"
                      "      \"role\": \"model\","
                      "      \"parts\": [{\"text\": \"Hello!\"}]"
                      "    }"
                      "  }],"
                      "  \"usageMetadata\": {"
                      "    \"promptTokenCount\": 100,"
                      "    \"candidatesTokenCount\": 5"
                      "  }"
                      "}";

   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   parsed_response_t out;
   agent_parse_response_gemini(root, &out);

   assert(out.prompt_tokens == 100);
   assert(out.completion_tokens == 5);
   assert(out.cache_read_tokens == 0);
   assert(out.cache_write_tokens == 0);

   agent_free_parsed_response(&out);
   cJSON_Delete(root);
   printf("parse_response_gemini_no_cache OK\n");
}

static void test_parse_response_gemini_tool_call(void)
{
   /* Gemini response with a function call */
   const char *json = "{"
                      "  \"candidates\": [{"
                      "    \"content\": {"
                      "      \"role\": \"model\","
                      "      \"parts\": [{"
                      "        \"functionCall\": {"
                      "          \"name\": \"bash\","
                      "          \"args\": {\"command\": \"ls\"}"
                      "        }"
                      "      }]"
                      "    }"
                      "  }],"
                      "  \"usageMetadata\": {"
                      "    \"promptTokenCount\": 200,"
                      "    \"candidatesTokenCount\": 20"
                      "  }"
                      "}";

   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   parsed_response_t out;
   agent_parse_response_gemini(root, &out);

   assert(out.is_tool_call == 1);
   assert(out.call_count == 1);
   assert(strcmp(out.calls[0].name, "bash") == 0);
   /* id must equal name for Gemini (used as tool_call_id in tool result messages) */
   assert(strcmp(out.calls[0].id, "bash") == 0);
   assert(out.calls[0].arguments != NULL);
   assert(strstr(out.calls[0].arguments, "ls") != NULL);
   assert(out.assistant_message != NULL);

   agent_free_parsed_response(&out);
   cJSON_Delete(root);
   printf("parse_response_gemini_tool_call OK\n");
}

static void test_parse_response_gemini_null_root(void)
{
   parsed_response_t out;
   agent_parse_response_gemini(NULL, &out);
   assert(out.prompt_tokens == 0);
   assert(out.is_tool_call == 0);
   assert(out.content == NULL);
   printf("parse_response_gemini_null_root OK\n");
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

   cJSON *req = agent_build_request_openai(&agent, messages, NULL, 32, 0.0);
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
   cJSON *req = agent_build_request_openai(&agent, messages, tools, 32, 0.0);
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
   cJSON *req = agent_build_request_openai(&agent, messages, tools, 32, 0.0);
   assert(req != NULL);
   assert(cJSON_GetObjectItem(req, "tools") == NULL);
   assert(cJSON_GetObjectItem(req, "chat_template_kwargs") == NULL);

   cJSON_Delete(req);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
   printf("openai_request_omits_empty_tools OK\n");
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

static void test_gemini_driver_has_prompt_cache_cap(void)
{
   delegate_drivers_init();
   const delegate_driver_t *d = delegate_driver_get("gemini");
   assert(d != NULL);

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.model, sizeof(agent.model), "gemini-1.5-pro");

   driver_caps_t caps;
   delegate_get_caps(d, &agent, &caps);
   assert(caps.capability_flags & DRIVER_CAP_PROMPT_CACHE);

   printf("gemini_driver_has_prompt_cache_cap OK\n");
}

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

int main(void)
{
   printf("test_agent_http: ");

   test_connect_timeout_caps_below_request_budget();

   test_cache_attach_adds_field();
   test_cache_attach_noop_on_empty_name();
   test_cache_attach_noop_on_null();

   test_cache_create_rejects_empty_inputs();

   test_build_request_gemini_uncached();
   test_build_request_gemini_cached();
   test_build_request_gemini_tool_result_conversion();
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
   test_agent_config_recommended_sampling_roundtrip();
   test_parse_response_openai_sanitizes_invalid_tool_arguments();
   test_parse_response_captures_provider_model();
   test_delegate_rescue_parses_mistral_bracket();
   test_delegate_rescue_parses_fenced_json();
   test_delegate_rescue_detects_invoke_markup();
   test_delegate_rescue_skips_malformed_invoke_markup();
   test_delegate_rescue_channel_balances_braces();
   test_delegate_rescue_strips_think_blocks();
   test_delegate_rescue_rejects_unknown_json_tool();
   test_parse_response_openai_mistral_content_array();
   test_parse_response_openai_stray_think_close();
   test_parse_response_openai_strips_thinking_block();
   test_parse_response_openai_strips_thinking_process_scaffold();
   test_parse_response_openai_strips_self_correction_scaffold();
   test_parse_response_openai_discards_private_scaffold_without_final();

   test_parse_response_gemini_cache_hit();
   test_parse_response_gemini_no_cache();
   test_parse_response_gemini_tool_call();
   test_parse_response_gemini_null_root();

   test_openai_request_llama_compat_options();
   test_openai_request_omits_empty_tools();
   test_provider_network_error_mentions_local_http_init();

   test_gemini_driver_has_prompt_cache_cap();
   test_minimax_driver_has_own_caps();

   printf("all tests passed\n");
   return 0;
}
