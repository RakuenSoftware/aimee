/* test_gateway_p4_delegate.c: universal-gateway P4 — the delegate/primary model-call
 * loop run through the gateway pipeline. Pure tests of gateway_delegate_* plus the
 * response-side police as the loop invokes it. config_load, guardrails_canonical_tool_name
 * and aimee_log are stubbed so the link stays minimal (their real impls have their own
 * tests). */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../headers/config.h"
#include "../headers/gateway_delegate.h"
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
/* LOG_WARN in gateway_delegate.c -> aimee_log; swallow it (and count, for the
 * gemini-warn assertion). */
static int g_warns = 0;
void aimee_log(int level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
   g_warns++;
}

static int has_tool_named(cJSON *req, const char *name, int openai)
{
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(req, "tools");
   cJSON *t;
   if (!cJSON_IsArray(tools))
      return 0;
   cJSON_ArrayForEach(t, tools)
   {
      cJSON *n = openai ? cJSON_GetObjectItem(cJSON_GetObjectItem(t, "function"), "name")
                        : cJSON_GetObjectItem(t, "name");
      if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
         return 1;
   }
   return 0;
}

/* (1) delegate + OpenAI (function-nested) shape, policy ON: Task stripped, benign kept. */
static void test_delegate_openai_strip(void)
{
   g_prevent = 1;
   cJSON *req = cJSON_Parse("{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"Read\"}},"
                            "{\"type\":\"function\",\"function\":{\"name\":\"Task\"}}]}");
   int n = gateway_delegate_run_request_pipeline(req, GW_TOOL_SHAPE_FUNCTION_NESTED, 1);
   assert(n == 1);
   assert(has_tool_named(req, "Read", 1));
   assert(!has_tool_named(req, "Task", 1));
   cJSON_Delete(req);
   PASS("delegate_openai_strip");
}

/* (2) delegate + flat NAMED shape (anthropic / responses), policy ON: Task stripped. */
static void test_delegate_named_strip(void)
{
   g_prevent = 1;
   cJSON *req = cJSON_Parse("{\"tools\":[{\"name\":\"Read\"},{\"name\":\"Task\"}]}");
   int n = gateway_delegate_run_request_pipeline(req, GW_TOOL_SHAPE_NAMED, 1);
   assert(n == 1);
   assert(has_tool_named(req, "Read", 0));
   assert(!has_tool_named(req, "Task", 0));
   cJSON_Delete(req);
   PASS("delegate_named_strip");
}

/* (3) PRIMARY (is_delegate=0), policy ON: Task PRESERVED, no intervention. */
static void test_primary_not_policed(void)
{
   g_prevent = 1;
   cJSON *req = cJSON_Parse("{\"tools\":[{\"name\":\"Read\"},{\"name\":\"Task\"}]}");
   int n = gateway_delegate_run_request_pipeline(req, GW_TOOL_SHAPE_NAMED, 0);
   assert(n == 0);
   assert(has_tool_named(req, "Task", 0)); /* primary keeps its delegation tool */
   cJSON_Delete(req);
   PASS("primary_not_policed");
}

/* (4) policy OFF (default): serialized request byte-identical pre/post, delegate+primary. */
static void test_policy_off_byte_identical(void)
{
   g_prevent = 0;
   const char *src = "{\"tools\":[{\"name\":\"Read\"},{\"name\":\"Task\"}],\"model\":\"m\"}";
   for (int is_delegate = 0; is_delegate <= 1; is_delegate++)
   {
      cJSON *req = cJSON_Parse(src);
      char *before = cJSON_PrintUnformatted(req);
      int n = gateway_delegate_run_request_pipeline(req, GW_TOOL_SHAPE_NAMED, is_delegate);
      char *after = cJSON_PrintUnformatted(req);
      assert(n == 0);
      assert(strcmp(before, after) == 0);
      free(before);
      free(after);
      cJSON_Delete(req);
   }
   PASS("policy_off_byte_identical");
}

/* Build a one-tool_use parsed_response_t with the given tool name (heap arguments,
 * so the drop path's free() is exercised). */
static void make_parsed(parsed_response_t *p, const char *tool)
{
   memset(p, 0, sizeof(*p));
   p->is_tool_call = 1;
   p->call_count = 1;
   snprintf(p->calls[0].name, sizeof(p->calls[0].name), "%s", tool);
   p->calls[0].arguments = strdup("{}");
   snprintf(p->stop_reason, sizeof(p->stop_reason), "tool_use");
}

/* (5) response-side police: denied tool_use dropped ON, kept OFF. Shape-agnostic. */
static void test_response_police(void)
{
   parsed_response_t p;

   g_prevent = 1;
   make_parsed(&p, "Task");
   int dropped = gateway_policy_police_parsed_response(&p);
   assert(dropped == 1);
   assert(p.call_count == 0);
   free(p.calls[0].arguments); /* survivors only; here none survive, but slot is reset */

   g_prevent = 0;
   make_parsed(&p, "Task");
   int kept = gateway_policy_police_parsed_response(&p);
   assert(kept == 0);
   assert(p.call_count == 1);
   free(p.calls[0].arguments);
   PASS("response_police");
}

/* (6) empty-after-strip: the lone Task tool gone -> tools array AND a forcing
 * tool_choice are dropped (no `tools: []` reaches the provider). */
static void test_empty_after_strip(void)
{
   g_prevent = 1;
   cJSON *req = cJSON_Parse("{\"tools\":[{\"name\":\"Task\"}],\"tool_choice\":{\"type\":\"any\"}}");
   int n = gateway_delegate_run_request_pipeline(req, GW_TOOL_SHAPE_NAMED, 1);
   assert(n == 1);
   assert(cJSON_GetObjectItemCaseSensitive(req, "tools") == NULL);
   assert(cJSON_GetObjectItemCaseSensitive(req, "tool_choice") == NULL);
   cJSON_Delete(req);
   PASS("empty_after_strip");
}

/* (7) gemini UNSUPPORTED shape, policy ON, delegate: req unchanged, warning emitted. */
static void test_gemini_unsupported_noop(void)
{
   g_prevent = 1;
   g_warns = 0;
   cJSON *req = cJSON_Parse("{\"tools\":[{\"functionDeclarations\":[{\"name\":\"Task\"}]}]}");
   char *before = cJSON_PrintUnformatted(req);
   int n = gateway_delegate_run_request_pipeline(req, GW_TOOL_SHAPE_UNSUPPORTED, 1);
   char *after = cJSON_PrintUnformatted(req);
   assert(n == 0);
   assert(strcmp(before, after) == 0); /* not policed */
   assert(g_warns == 1);               /* but observable, not silent */
   free(before);
   free(after);
   cJSON_Delete(req);
   PASS("gemini_unsupported_noop");
}

/* (8) the model field is never touched (model-pin is deliberately not run here). */
static void test_model_untouched(void)
{
   g_prevent = 1;
   cJSON *req = cJSON_Parse("{\"model\":\"fallback-model\",\"tools\":[{\"name\":\"Read\"}]}");
   gateway_delegate_run_request_pipeline(req, GW_TOOL_SHAPE_NAMED, 1);
   cJSON *m = cJSON_GetObjectItemCaseSensitive(req, "model");
   assert(cJSON_IsString(m) && strcmp(m->valuestring, "fallback-model") == 0);
   cJSON_Delete(req);
   PASS("model_untouched");
}

/* shape derivation maps each provider flag set to the right shape. */
static void test_shape_derivation(void)
{
   assert(gateway_delegate_tool_shape(1, 0) == GW_TOOL_SHAPE_NAMED);           /* anthropic */
   assert(gateway_delegate_tool_shape(0, 1) == GW_TOOL_SHAPE_NAMED);           /* responses */
   assert(gateway_delegate_tool_shape(0, 0) == GW_TOOL_SHAPE_FUNCTION_NESTED); /* openai */
   /* The gemini arm (-> GW_TOOL_SHAPE_UNSUPPORTED) is gone with the Gemini
    * provider: functionDeclarations was the only shape that could not name a tool
    * at tool.name, and Gemini is now reached over the OpenAI shape like anything
    * else. */
   PASS("shape_derivation");
}

int main(void)
{
   printf("gateway P4 delegate-unification tests:\n");
   test_shape_derivation();
   test_delegate_openai_strip();
   test_delegate_named_strip();
   test_primary_not_policed();
   test_policy_off_byte_identical();
   test_response_police();
   test_empty_after_strip();
   test_gemini_unsupported_noop();
   test_model_untouched();
   printf("ALL PASS\n");
   return 0;
}
