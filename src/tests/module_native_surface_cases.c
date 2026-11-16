#include "aimee.h"
#include <aimee/tools/agent_tools.h>
#include <aimee/ir/aimee_ir.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
/* Full server catalog API, independent forked startup states. No native-memory
 * or private authority is fabricated; the provider is a library test module. */
static int module_surface_calls;
static char *module_surface_call(const char *name, const cJSON *args, int timeout, int *error,
                                 void *state)
{
   (void)args;
   assert(!strcmp(name, "module_surface_sample") && timeout == 5000 &&
          state == &module_surface_calls);
   module_surface_calls++;
   *error = 0;
   return strdup("module test reply");
}
static const aimee_tool_t *module_surface_find(const aimee_request_t *catalog, const char *name)
{
   return aimee_ir_find_function(catalog, NULL, name);
}
static void module_surface_case(int which)
{
   cJSON *schema = cJSON_Parse("{\"type\":\"object\",\"properties\":{\"ref\":{\"type\":\"string\"}}"
                               ",\"required\":[\"ref\"],\"additionalProperties\":false}");
   assert(schema);
   agent_module_native_definition definition = {
       "module_surface_sample",
       "A module test definition",
       schema,
       {AIMEE_TOOL_CLASS_WRITE, AIMEE_TOOL_POLICY_WRITE, AIMEE_TOOL_POLICY_WRITE}};
   if (which == 0)
   {
      aimee_request_t catalog = {0};
      assert(!agent_tools_catalog(AIMEE_WIRE_OPENAI_CHAT, &catalog) &&
             !module_surface_find(&catalog, definition.name));
      aimee_request_free(&catalog);
      assert(agent_tools_module_native_install(&definition, 1, module_surface_call,
                                               &module_surface_calls) < 0);
   }
   else if (which == 1)
   {
      definition.name = "bash";
      assert(agent_tools_module_native_install(&definition, 1, module_surface_call,
                                               &module_surface_calls) < 0);
   }
   else
   {
      assert(!agent_tools_module_native_install(&definition, 1, module_surface_call,
                                                &module_surface_calls));
      if (which == 2)
      {
         const aimee_wire_t wires[] = {AIMEE_WIRE_OPENAI_CHAT, AIMEE_WIRE_RESPONSES,
                                       AIMEE_WIRE_ANTHROPIC};
         for (size_t i = 0; i < sizeof wires / sizeof *wires; i++)
         {
            aimee_request_t catalog = {0};
            assert(!agent_tools_catalog(wires[i], &catalog));
            const aimee_tool_t *found = module_surface_find(&catalog, definition.name);
            assert(found && cJSON_Compare(found->schema, schema, 1));
            aimee_request_free(&catalog);
         }
         aimee_tool_t current = {0};
         assert(!agent_tools_execution_definition(definition.name, &current) &&
                agent_tools_module_native_admitted(&current));
         aimee_tool_free(&current);
      }
      if (which == 3)
      {
         agent_tools_register_mcp_tool(definition.name);
         aimee_request_t catalog = {0};
         assert(agent_tools_catalog(AIMEE_WIRE_OPENAI_CHAT, &catalog));
         aimee_request_free(&catalog);
         int error = 0;
         char *reply =
             agent_tools_module_native_call(definition.name, "{\"ref\":\"safe\"}", 5000, &error);
         assert(error && reply && !module_surface_calls);
         free(reply);
      }
      if (which == 4)
      {
         int error = 1;
         char *reply =
             agent_tools_module_native_call(definition.name, "{\"ref\":\"safe\"}", 5000, &error);
         assert(!error && reply && module_surface_calls == 1);
         free(reply);
      }
   }
   cJSON_Delete(schema);
}
void module_surface_cases(void)
{
   for (int i = 0; i < 5; i++)
   {
      pid_t child = fork();
      assert(child >= 0);
      if (!child)
      {
         alarm(10);
         module_surface_case(i);
         _exit(0);
      }
      int status = 0;
      assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
   }
   puts("  PASS: module-native defaultoff, collisions, all three catalogs and owned callback");
}
