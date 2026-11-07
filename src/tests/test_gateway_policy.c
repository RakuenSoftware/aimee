/* test_gateway_policy.c: pure tests for gateway request-side tool policing.
 * Accessors and guardrails_is_subagent_tool are stubbed so the link is minimal
 * (the real ones are covered by their own modules' tests). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include <aimee/gateway/gateway_policy.h>

#define PASS(name) printf("  PASS: %s\n", (name))

static int g_prevent = 0;
/* Accessor stubs expose the policy values under test. */
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

static void test_canonical_empty_and_surviving_choice(void)
{
   assert(!gateway_policy_apply_ir(NULL));
   aimee_request_t request = {.n_tools = 2,
                              .tool_choice = AIMEE_TOOL_CHOICE_TOOL,
                              .tool_choice_name = strdup("Read"),
                              .has_parallel_tool_calls = 1};
   request.tools = calloc(2, sizeof *request.tools);
   assert(request.tools);
   request.tools[0].name = strdup("Task");
   request.tools[1].name = strdup("Read");
   g_prevent = 1;
   assert(gateway_policy_apply_ir(&request) == 1 && request.n_tools == 1 &&
          !strcmp(request.tools[0].name, "Read") && request.tool_choice == AIMEE_TOOL_CHOICE_TOOL &&
          !strcmp(request.tool_choice_name, "Read") && request.has_parallel_tool_calls);
   aimee_request_free(&request);
   request = (aimee_request_t){
       .n_tools = 1, .tool_choice = AIMEE_TOOL_CHOICE_ANY, .has_parallel_tool_calls = 1};
   request.tools = calloc(1, sizeof *request.tools);
   assert(request.tools);
   request.tools[0].name = strdup("Agent");
   assert(gateway_policy_apply_ir(&request) == 1 && !request.n_tools &&
          request.tool_choice == AIMEE_TOOL_CHOICE_UNSET && !request.has_parallel_tool_calls);
   assert(!gateway_policy_apply_ir(&request));
   aimee_request_free(&request);
   PASS("canonical empty catalog clears choice; surviving forced tool stays exact");
}

int main(void)
{
   printf("test_gateway_policy:\n");
   test_canonical_namespace_policing();
   test_canonical_empty_and_surviving_choice();

   /* enforce-delegate-only: a registered delegates-available provider activates
    * sub-agent prevention even when the config flag is off. */
   gateway_policy_set_delegates_available_provider(test_delegates_available_yes);
   assert(gateway_prevent_subagents_enabled() == 1);
   gateway_policy_set_delegates_available_provider(NULL);

   printf("all gateway_policy tests passed\n");
   return 0;
}
