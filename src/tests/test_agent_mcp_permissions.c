/* Permission ceilings must survive the same bridge/family resolution used by
 * served native calls. No tool handler runs in these admission checks. */
#include <aimee/tools/agent_tools.h>
#include <aimee/protocols/mcp/mcp_tools.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

void test_native_mcp_permission_admission(void)
{
   agent_tools_scope_snapshot_t previous, captured;
   agent_tools_scope_bookmark(&previous);
   static const char *const denied[] = {"git_push", "create_note", "vendor:mutate"};
   agent_tools_set_dispatch_role(NULL);
   agent_tools_set_active_toolset(NULL);
   agent_tools_shell_set(0);
   agent_tools_knowledge_write_set(1);
   agent_tools_denied_set(denied, 3);
   assert(!agent_tools_scope_capture_owned(&captured));
   agent_tools_scope_restore(&captured);
   assert(agent_tools_scope_isolated());

   cJSON *args = cJSON_Parse("{\"command\":\"push\",\"remote\":\"origin\"}");
   assert(args);
   assert(agent_tools_permission_check("git_push", args) == AGENT_TOOL_PERMISSION_ROLE);
   char member[96];
   assert(mcp_family_demux("git", args, member, sizeof member) == 1);
   assert(!strcmp(member, "git_push"));
   assert(agent_tools_permission_check(member, args) == AGENT_TOOL_PERMISSION_ROLE);
   cJSON *wrapper = cJSON_CreateObject();
   cJSON_AddStringToObject(wrapper, "name", "git");
   cJSON_AddItemToObject(wrapper, "arguments", args);
   const char *target = NULL;
   cJSON *target_args = NULL;
   assert(mcp_call_tool_demux("call_tool", wrapper, &target, &target_args) == 1);
   assert(target_args == args);
   assert(mcp_family_demux(target, target_args, member, sizeof member) == 1);
   assert(agent_tools_permission_check(member, target_args) == AGENT_TOOL_PERMISSION_ROLE);
   cJSON_Delete(wrapper);

   args = cJSON_Parse("{\"command\":\"create\"}");
   assert(mcp_family_demux("note", args, member, sizeof member) == 1);
   assert(agent_tools_permission_check(member, args) == AGENT_TOOL_PERMISSION_ROLE);
   assert(agent_tools_permission_check("vendor:mutate", args) == AGENT_TOOL_PERMISSION_ROLE);
   assert(agent_tools_permission_check("bash", args) == AGENT_TOOL_PERMISSION_SHELL);
   assert(agent_tools_permission_check("execute_script", args) == AGENT_TOOL_PERMISSION_SHELL);
   assert(agent_tools_permission_check("read_file", args) == AGENT_TOOL_PERMISSION_ALLOWED);
   cJSON_Delete(args);

   /* Permission checks must also honor the captured toolset, independently of
    * denied names, and retain the shell knowledge restriction. */
   agent_tools_set_active_toolset("review_indexed");
   assert(agent_tools_permission_check("read_file", NULL) == AGENT_TOOL_PERMISSION_ALLOWED);
   assert(agent_tools_permission_check("write_file", NULL) == AGENT_TOOL_PERMISSION_ROLE);
   agent_tools_set_active_toolset(NULL);
   agent_tools_shell_set(1);
   agent_tools_knowledge_write_set(0);
   args = cJSON_Parse("{\"command\":\"aimee index ingest\"}");
   assert(agent_tools_permission_check("bash", args) == AGENT_TOOL_PERMISSION_KNOWLEDGE);
   cJSON_DeleteItemFromObjectCaseSensitive(args, "command");
   cJSON_AddStringToObject(args, "Command", "aimee index ingest");
   assert(agent_tools_permission_check("bash", args) == AGENT_TOOL_PERMISSION_KNOWLEDGE);
   cJSON_DeleteItemFromObjectCaseSensitive(args, "Command");
   cJSON_AddStringToObject(args, "command", "aimee index find main");
   assert(agent_tools_permission_check("bash", args) == AGENT_TOOL_PERMISSION_ALLOWED);
   cJSON_Delete(args);

   agent_tools_scope_restore(&previous);
   agent_tools_scope_destroy_owned(&captured);
   puts("  PASS: native MCP bridge/family calls retain captured permission ceilings");
}
