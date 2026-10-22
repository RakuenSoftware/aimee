/* test_gateway_policy.c: pure tests for gateway request-side tool policing.
 * Accessors and guardrails_is_subagent_tool are stubbed so the link is minimal
 * (the real ones are covered by their own modules' tests). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include <aimee/gateway/gateway_policy.h>
#include "../vendor/headers/cJSON.h"

#define PASS(name) printf("  PASS: %s\n", (name))

static int g_prevent = 0;
static int g_pin = 0;
/* Accessor stubs expose the policy values under test. */
int config_gateway_pin_model(void)
{
   return g_pin;
}

int config_gateway_prevent_subagents(void)
{
   return g_prevent;
}
const char *guardrails_canonical_tool_name(const char *n)
{
   if (n && (strcmp(n, "Task") == 0 || strcmp(n, "Agent") == 0))
      return "Subagent";
   return n ? n : "";
}

static int has_tool(cJSON *req, const char *name, int openai)
{
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(req, "tools");
   cJSON *t;
   if (!cJSON_IsArray(tools))
      return 0;
   cJSON_ArrayForEach(t, tools)
   {
      const char *n =
          openai ? cJSON_GetObjectItem(cJSON_GetObjectItem(t, "function"), "name")->valuestring
                 : cJSON_GetObjectItem(t, "name")->valuestring;
      if (n && strcmp(n, name) == 0)
         return 1;
   }
   return 0;
}

/* Anthropic shape: subagent tool stripped, other tools kept, forced tool_choice relaxed. */
static void test_anthropic_strip(void)
{
   g_prevent = 1;
   cJSON *req = cJSON_Parse("{\"tools\":[{\"name\":\"Read\",\"input_schema\":{}},"
                            "{\"name\":\"Task\",\"input_schema\":{}}],"
                            "\"tool_choice\":{\"type\":\"tool\",\"name\":\"Task\"}}");
   int n = gateway_policy_apply_request(req, 0);
   assert(n == 1);
   assert(has_tool(req, "Read", 0));
   assert(!has_tool(req, "Task", 0));
   assert(cJSON_GetObjectItemCaseSensitive(req, "tool_choice") == NULL); /* relaxed to auto */
   cJSON_Delete(req);
   PASS("anthropic_strip");
}

/* Empty-after-strip: tools array AND a now-meaningless tool_choice are dropped
 * (a `required`/`any` choice with no tools would 400 upstream). */
static void test_empty_tools_dropped(void)
{
   g_prevent = 1;
   cJSON *req = cJSON_Parse(
       "{\"tools\":[{\"name\":\"Agent\",\"input_schema\":{}}],\"tool_choice\":{\"type\":\"any\"}}");
   int n = gateway_policy_apply_request(req, 0);
   assert(n == 1);
   assert(cJSON_GetObjectItemCaseSensitive(req, "tools") == NULL);
   assert(cJSON_GetObjectItemCaseSensitive(req, "tool_choice") == NULL);
   cJSON_Delete(req);
   PASS("empty_tools_dropped");
}

/* OpenAI shape: function-name match. */
static void test_openai_strip(void)
{
   g_prevent = 1;
   cJSON *req = cJSON_Parse("{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"Read\"}},"
                            "{\"type\":\"function\",\"function\":{\"name\":\"Task\"}}]}");
   int n = gateway_policy_apply_request(req, 1);
   assert(n == 1);
   assert(has_tool(req, "Read", 1));
   assert(!has_tool(req, "Task", 1));
   cJSON_Delete(req);
   PASS("openai_strip");
}

/* Policy off: no-op, request untouched. */
static void test_policy_off_noop(void)
{
   g_prevent = 0;
   cJSON *req = cJSON_Parse("{\"tools\":[{\"name\":\"Task\",\"input_schema\":{}}]}");
   int n = gateway_policy_apply_request(req, 0);
   assert(n == 0);
   assert(has_tool(req, "Task", 0));
   cJSON_Delete(req);
   PASS("policy_off_noop");
}

/* Bare-array strip (OpenAI /v1/responses seam): subagent removed in place, count
 * returned, surviving tools kept, no enclosing req/tool_choice touched. */
static void test_strip_tools_bare_array(void)
{
   g_prevent = 1;
   cJSON *tools = cJSON_Parse("[{\"type\":\"function\",\"function\":{\"name\":\"Read\"}},"
                              "{\"type\":\"function\",\"function\":{\"name\":\"Task\"}}]");
   int n = gateway_policy_strip_tools(tools, 1);
   assert(n == 1);
   assert(cJSON_GetArraySize(tools) == 1);
   assert(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(tools, 0), "function")->child->valuestring,
                 "Read") == 0);
   cJSON_Delete(tools);
   PASS("strip_tools_bare_array");
}

/* Bare-array strip, all removed: returns count, leaves an empty array (the caller
 * is responsible for omitting it from the provider request). */
static void test_strip_tools_emptied(void)
{
   g_prevent = 1;
   cJSON *tools = cJSON_Parse("[{\"type\":\"function\",\"function\":{\"name\":\"Agent\"}}]");
   int n = gateway_policy_strip_tools(tools, 1);
   assert(n == 1);
   assert(cJSON_GetArraySize(tools) == 0);
   cJSON_Delete(tools);
   PASS("strip_tools_emptied");
}

/* Bare-array strip, policy off / NULL: no-op. */
static void test_strip_tools_off_and_null(void)
{
   g_prevent = 0;
   cJSON *tools = cJSON_Parse("[{\"type\":\"function\",\"function\":{\"name\":\"Task\"}}]");
   assert(gateway_policy_strip_tools(tools, 1) == 0);
   assert(cJSON_GetArraySize(tools) == 1);
   cJSON_Delete(tools);
   g_prevent = 1;
   assert(gateway_policy_strip_tools(NULL, 1) == 0);
   PASS("strip_tools_off_and_null");
}

/* Model-pin: on -> the served model is forced to the agent's model. */
static void test_pin_model_on_swaps(void)
{
   cJSON *req = cJSON_Parse("{\"model\":\"claude-opus-from-client\",\"max_tokens\":8}");
   g_pin = 1;
   int changed = gateway_policy_pin_model(req, "primary-model");
   assert(changed == 1);
   assert(strcmp(cJSON_GetObjectItem(req, "model")->valuestring, "primary-model") == 0);
   g_pin = 0;
   cJSON_Delete(req);
   PASS("pin_model_on_swaps");
}

/* Model-pin off (default) is a byte-neutral no-op: the client model is honored. */
static void test_pin_model_off_noop(void)
{
   cJSON *req = cJSON_Parse("{\"model\":\"client-model\"}");
   g_pin = 0;
   int changed = gateway_policy_pin_model(req, "primary-model");
   assert(changed == 0);
   assert(strcmp(cJSON_GetObjectItem(req, "model")->valuestring, "client-model") == 0);
   cJSON_Delete(req);
   PASS("pin_model_off_noop");
}

/* Pin on but the request already names the pinned model, or no agent model -> no-op. */
static void test_pin_model_idempotent_and_guarded(void)
{
   cJSON *req = cJSON_Parse("{\"model\":\"primary-model\"}");
   g_pin = 1;
   assert(gateway_policy_pin_model(req, "primary-model") == 0);  /* already pinned */
   assert(gateway_policy_pin_model(req, "") == 0);               /* empty agent model */
   assert(gateway_policy_pin_model(NULL, "primary-model") == 0); /* null req */
   assert(strcmp(cJSON_GetObjectItem(req, "model")->valuestring, "primary-model") == 0);
   g_pin = 0;
   cJSON_Delete(req);
   PASS("pin_model_idempotent_and_guarded");
}

/* --- Response-side tool policing (P2c). The stubbed guardrails_canonical_tool_name
 * (line 26) maps Task/Agent to "Subagent"; `is_subagent_tool_name` (used internally
 * by the predicate + the police function) therefore denies those names. */

/* Predicate sanity: gateway_policy_is_denied_tool reads the gate itself. */
static void test_predicate_is_denied_tool(void)
{
   g_prevent = 1;
   assert(gateway_policy_is_denied_tool("Task") == 1);
   assert(gateway_policy_is_denied_tool("Agent") == 1);
   assert(gateway_policy_is_denied_tool("web_search") == 0);
   assert(gateway_policy_is_denied_tool("") == 0);
   assert(gateway_policy_is_denied_tool(NULL) == 0);
   g_prevent = 0;
   assert(gateway_policy_is_denied_tool("Task") == 0); /* gate off */
   PASS("predicate_is_denied_tool");
}

static int test_delegates_available_yes(void)
{
   return 1;
}

static void test_canonical_namespace_policing(void)
{
   aimee_request_t request = {
       .n_tools = 2, .tool_choice = AIMEE_TOOL_CHOICE_TOOL, .tool_choice_name = strdup("Task")};
   request.tools = calloc(2, sizeof *request.tools);
   assert(request.tools);
   request.tools[0] = (aimee_tool_t){
       .kind = AIMEE_TOOL_NAMESPACE, .name = strdup("mcp__fixture"), .n_children = 2};
   request.tools[0].children = calloc(2, sizeof *request.tools[0].children);
   assert(request.tools[0].children);
   request.tools[0].children[0].name = strdup("Task");
   request.tools[0].children[1].name = strdup("Read");
   request.tools[1] = (aimee_tool_t){.kind = AIMEE_TOOL_HOSTED,
                                     .provider_wire = AIMEE_WIRE_RESPONSES,
                                     .native_type = strdup("web_search")};
   g_prevent = 0;
   assert(!gateway_policy_apply_ir(&request));
   g_prevent = 1;
   assert(gateway_policy_apply_ir(&request) == 1);
   assert(request.mutated && request.n_tools == 2 && request.tools[0].n_children == 1);
   assert(!strcmp(request.tools[0].children[0].name, "Read"));
   assert(request.tools[1].kind == AIMEE_TOOL_HOSTED &&
          request.tool_choice == AIMEE_TOOL_CHOICE_AUTO);
   assert(!gateway_policy_apply_ir(&request));
   aimee_request_free(&request);
   PASS("canonical namespace children policed without losing hosted tools");
}

int main(void)
{
   printf("test_gateway_policy:\n");
   test_canonical_namespace_policing();
   test_anthropic_strip();
   test_empty_tools_dropped();
   test_openai_strip();
   test_policy_off_noop();
   test_strip_tools_bare_array();
   test_strip_tools_emptied();
   test_strip_tools_off_and_null();
   test_pin_model_on_swaps();
   test_pin_model_off_noop();
   test_pin_model_idempotent_and_guarded();
   test_predicate_is_denied_tool();

   /* enforce-delegate-only: a registered delegates-available provider activates
    * sub-agent prevention even when the config flag is off. */
   gateway_policy_set_delegates_available_provider(test_delegates_available_yes);
   assert(gateway_prevent_subagents_enabled() == 1);
   gateway_policy_set_delegates_available_provider(NULL);

   printf("all gateway_policy tests passed\n");
   return 0;
}
