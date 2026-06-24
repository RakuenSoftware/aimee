/* test_gateway_policy.c: pure tests for gateway request-side tool policing.
 * config_load and guardrails_is_subagent_tool are stubbed so the link is minimal
 * (the real ones are covered by their own modules' tests). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../headers/config.h"
#include "../headers/gateway_policy.h"
#include "../vendor/headers/cJSON.h"

#define PASS(name) printf("  PASS: %s\n", (name))

static int g_prevent = 0;
int config_load(config_t *cfg)
{
   if (cfg)
   {
      memset(cfg, 0, sizeof(*cfg));
      cfg->gateway_prevent_subagents = g_prevent;
   }
   return 0;
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

int main(void)
{
   printf("test_gateway_policy:\n");
   test_anthropic_strip();
   test_empty_tools_dropped();
   test_openai_strip();
   test_policy_off_noop();
   test_strip_tools_bare_array();
   test_strip_tools_emptied();
   test_strip_tools_off_and_null();
   printf("all gateway_policy tests passed\n");
   return 0;
}
