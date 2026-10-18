/* Delegate request policy operates on the same canonical catalog for all wires. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <aimee/gateway/gateway_delegate.h>

static int g_prevent;
int config_gateway_prevent_subagents(void)
{
   return g_prevent;
}
const char *guardrails_canonical_tool_name(const char *name)
{
   if (name && (!strcmp(name, "Task") || !strcmp(name, "Agent")))
      return "Subagent";
   return name ? name : "";
}

static aimee_request_t request(void)
{
   aimee_request_t req = {.model = strdup("selected-fallback-model"),
                          .n_tools = 2,
                          .tool_choice = AIMEE_TOOL_CHOICE_ANY};
   req.tools = calloc(2, sizeof *req.tools);
   assert(req.tools && req.model);
   req.tools[0].name = strdup("Read");
   req.tools[1].name = strdup("Task");
   assert(req.tools[0].name && req.tools[1].name);
   return req;
}

int main(void)
{
   g_prevent = 1;
   aimee_request_t req = request();
   assert(gateway_delegate_run_request_pipeline(&req, 1) == 1);
   assert(req.n_tools == 1 && !strcmp(req.tools[0].name, "Read"));
   assert(req.tool_choice == AIMEE_TOOL_CHOICE_ANY);
   assert(!strcmp(req.model, "selected-fallback-model"));
   assert(req.mutated);
   assert(!gateway_delegate_run_request_pipeline(&req, 1));
   aimee_request_free(&req);

   req = request();
   assert(!gateway_delegate_run_request_pipeline(&req, 0));
   assert(req.n_tools == 2 && !req.mutated);
   aimee_request_free(&req);

   g_prevent = 0;
   req = request();
   assert(!gateway_delegate_run_request_pipeline(&req, 1));
   assert(req.n_tools == 2 && !req.mutated);
   aimee_request_free(&req);

   g_prevent = 1;
   req = request();
   req.tool_choice = AIMEE_TOOL_CHOICE_TOOL;
   req.tool_choice_name = strdup("Task");
   assert(gateway_delegate_run_request_pipeline(&req, 1) == 1);
   assert(req.tool_choice == AIMEE_TOOL_CHOICE_AUTO && !req.tool_choice_name);
   aimee_request_free(&req);

   req = request();
   free(req.tools[0].name);
   req.tools[0].name = strdup("Agent");
   req.has_parallel_tool_calls = 1;
   assert(gateway_delegate_run_request_pipeline(&req, 1) == 2);
   assert(!req.n_tools && req.tool_choice == AIMEE_TOOL_CHOICE_UNSET);
   assert(!req.has_parallel_tool_calls);
   aimee_request_free(&req);

   /* Namespace children share canonical policy; hosted capabilities survive. */
   req = request();
   aimee_tool_t child = req.tools[1];
   req.tools[1] = (aimee_tool_t){.kind = AIMEE_TOOL_NAMESPACE,
                                 .name = strdup("native"),
                                 .children = malloc(sizeof child),
                                 .n_children = 1};
   assert(req.tools[1].children);
   req.tools[1].children[0] = child;
   assert(gateway_delegate_run_request_pipeline(&req, 1) == 2);
   assert(req.n_tools == 1 && !strcmp(req.tools[0].name, "Read"));
   aimee_request_free(&req);
   assert(gateway_delegate_run_request_pipeline(NULL, 1) < 0);
   puts("gateway P4 canonical delegate policy: all tests passed");
   return 0;
}
