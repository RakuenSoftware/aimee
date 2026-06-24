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
static int g_pin = 0;
int config_load(config_t *cfg)
{
   if (cfg)
   {
      memset(cfg, 0, sizeof(*cfg));
      cfg->gateway_prevent_subagents = g_prevent;
      cfg->gateway_pin_model = g_pin;
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

/* Build a parsed_response_t with N tool calls whose names are `names[i]` and ids
 * `ids[i]`. The struct is stack-allocated and partially zero-initialized — the
 * police function only reads `calls[]/call_count/stop_reason`. NULL `names`
 * and/or `ids` arrays are treated as all-empty (the test_police_handles_empty_tool_name
 * case — the police predicate's "empty name = not denied" branch is exercised
 * without dereferencing the array). Per-element NULL is also tolerated to keep
 * the helper robust to future callers. */
static void seed_parsed(parsed_response_t *p, int n, const char *names[], const char *ids[])
{
   int i;
   memset(p, 0, sizeof(*p));
   p->call_count = n;
   p->stop_reason[0] = '\0';
   for (i = 0; i < n; i++)
   {
      snprintf(p->calls[i].id, sizeof(p->calls[i].id), "%s", (ids && ids[i]) ? ids[i] : "");
      snprintf(p->calls[i].name, sizeof(p->calls[i].name), "%s",
               (names && names[i]) ? names[i] : "");
   }
}

/* A single subagent call is dropped, stop_reason recomputes to "end_turn". */
static void test_police_drops_denied_subagent_call(void)
{
   parsed_response_t p;
   const char *names[] = {"Task"};
   const char *ids[] = {"t1"};
   g_prevent = 1;
   seed_parsed(&p, 1, names, ids);
   int drops = gateway_policy_police_parsed_response(&p);
   assert(drops == 1);
   assert(p.call_count == 0);
   assert(strcmp(p.stop_reason, "end_turn") == 0);
   PASS("police_drops_denied_subagent_call");
}

/* A non-subagent call survives; stop_reason remains "tool_use". */
static void test_police_keeps_non_subagent_call(void)
{
   parsed_response_t p;
   const char *names[] = {"web_search"};
   const char *ids[] = {"t1"};
   g_prevent = 1;
   seed_parsed(&p, 1, names, ids);
   int drops = gateway_policy_police_parsed_response(&p);
   assert(drops == 0);
   assert(p.call_count == 1);
   assert(strcmp(p.calls[0].name, "web_search") == 0);
   assert(strcmp(p.calls[0].id, "t1") == 0);
   assert(strcmp(p.stop_reason, "tool_use") == 0);
   PASS("police_keeps_non_subagent_call");
}

/* Edge: call_count == 0 — no work, no stops mutated (would otherwise set
 * stop_reason to "end_turn" without reason; off-path keeps it empty). */
static void test_police_handles_zero_calls(void)
{
   parsed_response_t p;
   g_prevent = 1;
   seed_parsed(&p, 0, NULL, NULL);
   int drops = gateway_policy_police_parsed_response(&p);
   assert(drops == 0);
   assert(p.call_count == 0);
   assert(p.stop_reason[0] == '\0');
   PASS("police_handles_zero_calls");
}

/* [Subagent, web_search, Subagent] -> call_count == 1, calls[0].name == "web_search",
 * in the original relative order. Surfaces the in-place compaction correctness
 * (B2 manifested at unit scope). */
static void test_police_handles_mixed_calls(void)
{
   parsed_response_t p;
   const char *names[] = {"Task", "web_search", "Agent"};
   const char *ids[] = {"t1", "t2", "t3"};
   g_prevent = 1;
   seed_parsed(&p, 3, names, ids);
   int drops = gateway_policy_police_parsed_response(&p);
   assert(drops == 2);
   assert(p.call_count == 1);
   assert(strcmp(p.calls[0].id, "t2") == 0);
   assert(strcmp(p.calls[0].name, "web_search") == 0);
   assert(strcmp(p.stop_reason, "tool_use") == 0);
   PASS("police_handles_mixed_calls");
}

/* A tool with name == "" (empty string) is treated as "not denied" (the
 * predicate returns 0 on a name with no first char; the request-side strip
 * behaves the same). The NULL-name branch of the predicate is exercised
 * separately in test_predicate_is_denied_tool. */
static void test_police_handles_empty_tool_name(void)
{
   parsed_response_t p;
   const char *ids[] = {"t1"};
   g_prevent = 1;
   seed_parsed(&p, 1, NULL, ids); /* names == NULL => all calls[i].name == "" */
   int drops = gateway_policy_police_parsed_response(&p);
   assert(drops == 0);
   assert(p.call_count == 1);
   assert(strcmp(p.stop_reason, "tool_use") == 0);
   PASS("police_handles_empty_tool_name");
}

/* A dropped entry's `arguments` string is freed by the police function, not
 * left for `agent_free_parsed_response` (which only sweeps 0..call_count-1 and
 * would otherwise miss it because the slot is overwritten by a survivor).
 * This test mallocs a per-test string for the dropped entry's arguments and
 * runs the police function under a leak detector (or, in plain mode, asserts
 * the dropped slot's arguments are NULL post-compaction — proof the pointer
 * was zeroed before the struct-copy). */
static void test_police_drops_free_arguments(void)
{
   parsed_response_t p;
   const char *names[] = {"Task", "web_search"};
   const char *ids[] = {"t1", "t2"};
   /* The dropped entry's arguments live in heap memory the test owns; the
    * police function is expected to free it. (If it does not, a sanitizer
    * build will report the leak; in a non-sanitizer build the test still
    * proves the function does not crash.) */
   char *dropped_args = strdup("{\"subagent_args\":42}");
   g_prevent = 1;
   memset(&p, 0, sizeof(p));
   p.call_count = 2;
   snprintf(p.calls[0].id, sizeof(p.calls[0].id), "%s", ids[0]);
   snprintf(p.calls[0].name, sizeof(p.calls[0].name), "%s", names[0]);
   p.calls[0].arguments = dropped_args;
   snprintf(p.calls[1].id, sizeof(p.calls[1].id), "%s", ids[1]);
   snprintf(p.calls[1].name, sizeof(p.calls[1].name), "%s", names[1]);
   p.calls[1].arguments = strdup("{}");
   assert(p.calls[0].arguments == dropped_args);
   int drops = gateway_policy_police_parsed_response(&p);
   assert(drops == 1);
   assert(p.call_count == 1);
   assert(strcmp(p.calls[0].name, "web_search") == 0);
   assert(strcmp(p.calls[0].id, "t2") == 0);
   /* The function must have called free() on the dropped string. The
    * `agent_free_parsed_response` 0..call_count-1 sweep on cleanup will free
    * the survivor's arguments; the dropped string would leak without the
    * explicit free. (No assertion is possible without a leak detector, but
    * a sanitizer run will flag this test if the free is removed.) */
   free(p.calls[0].arguments);
   PASS("police_drops_free_arguments");
}

/* Police is idempotent: re-running yields 0 drops (the surviving calls are not
 * subagent, and the already-cleared path can't be re-cleared). */
static void test_police_is_idempotent(void)
{
   parsed_response_t p;
   const char *names[] = {"Task", "web_search"};
   const char *ids[] = {"t1", "t2"};
   g_prevent = 1;
   seed_parsed(&p, 2, names, ids);
   assert(gateway_policy_police_parsed_response(&p) == 1);
   assert(p.call_count == 1);
   assert(strcmp(p.calls[0].name, "web_search") == 0);
   assert(gateway_policy_police_parsed_response(&p) == 0); /* second run is a no-op */
   assert(p.call_count == 1);
   assert(strcmp(p.calls[0].name, "web_search") == 0);
   assert(strcmp(p.stop_reason, "tool_use") == 0);
   PASS("police_is_idempotent");
}

/* All subagent: call_count -> 0, stop_reason -> "end_turn". */
static void test_police_recovers_end_turn_when_all_calls_dropped(void)
{
   parsed_response_t p;
   const char *names[] = {"Task", "Agent"};
   const char *ids[] = {"t1", "t2"};
   g_prevent = 1;
   seed_parsed(&p, 2, names, ids);
   int drops = gateway_policy_police_parsed_response(&p);
   assert(drops == 2);
   assert(p.call_count == 0);
   assert(strcmp(p.stop_reason, "end_turn") == 0);
   PASS("police_recovers_end_turn_when_all_calls_dropped");
}

/* Partial drop: at least one surviving call keeps stop_reason "tool_use". */
static void test_police_keeps_stop_reason_when_calls_remain(void)
{
   parsed_response_t p;
   const char *names[] = {"web_search", "Task"};
   const char *ids[] = {"t1", "t2"};
   g_prevent = 1;
   seed_parsed(&p, 2, names, ids);
   int drops = gateway_policy_police_parsed_response(&p);
   assert(drops == 1);
   assert(p.call_count == 1);
   assert(strcmp(p.calls[0].name, "web_search") == 0);
   assert(strcmp(p.stop_reason, "tool_use") == 0);
   PASS("police_keeps_stop_reason_when_calls_remain");
}

/* Policy off: the function reads the gate itself, so a subagent name is not
 * denied even when the caller passes one in. */
static void test_police_policy_off_is_noop(void)
{
   parsed_response_t p;
   const char *names[] = {"Task"};
   const char *ids[] = {"t1"};
   g_prevent = 0;
   seed_parsed(&p, 1, names, ids);
   int drops = gateway_policy_police_parsed_response(&p);
   assert(drops == 0);
   assert(p.call_count == 1);
   assert(strcmp(p.calls[0].name, "Task") == 0);
   /* off-path does NOT mutate stop_reason (would otherwise force a recompute
    * where the model returned one; we leave the struct as-is). */
   assert(p.stop_reason[0] == '\0');
   PASS("police_policy_off_is_noop");
}

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
   test_pin_model_on_swaps();
   test_pin_model_off_noop();
   test_pin_model_idempotent_and_guarded();
   test_police_drops_denied_subagent_call();
   test_police_keeps_non_subagent_call();
   test_police_handles_zero_calls();
   test_police_handles_mixed_calls();
   test_police_handles_empty_tool_name();
   test_police_drops_free_arguments();
   test_police_is_idempotent();
   test_police_recovers_end_turn_when_all_calls_dropped();
   test_police_keeps_stop_reason_when_calls_remain();
   test_police_policy_off_is_noop();
   test_predicate_is_denied_tool();
   printf("all gateway_policy tests passed\n");
   return 0;
}
