/* Native admission checks preserve permission ceilings through bridge/family
 * resolution. Script handoff checks exercise the real joined RPC dispatcher. */
#include <aimee/tools/agent_tools.h>
#include <aimee/protocols/mcp/mcp_tools.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "platform_test_util.h"

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

void test_script_rpc_keeps_permission_scope(void)
{
   agent_tools_scope_snapshot_t previous;
   agent_tools_scope_bookmark(&previous);
   static const char *const denied[] = {"read_file"};
   agent_tools_set_dispatch_role(NULL);
   agent_tools_set_active_toolset(NULL);
   agent_tools_denied_set(denied, 1);
   char directory[MAX_PATH_LEN], path[MAX_PATH_LEN], body[MAX_PATH_LEN * 2];
   snprintf(directory, sizeof directory, "%s/ars.XXXXXX", platform_tmpdir());
   char *created = platform_mkdtemp(directory);
   assert(created);
   snprintf(path, sizeof path, "%s/evidence.txt", directory);
   FILE *file = fopen(path, "w");
   assert(file);
   int written = fputs("SCRIPT_SCOPE_PRIVATE_EVIDENCE", file);
   int closed = fclose(file);
   assert(written >= 0 && !closed);
   cJSON *literal = cJSON_CreateString(path);
   char *quoted = cJSON_PrintUnformatted(literal);
   cJSON_Delete(literal);
   assert(quoted);
   int length = snprintf(body, sizeof body,
                         "import json, aimee_tools\n"
                         "print(json.dumps(aimee_tools.call('read_file', {'path': %s})))\n",
                         quoted);
   free(quoted);
   assert(length > 0 && (size_t)length < sizeof body);
   char *result = tool_execute_script("python", body, 5, directory, NULL);
   cJSON *reply = result ? cJSON_Parse(result) : NULL;
   assert(reply && cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0);
   const char *output = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "stdout"));
   assert(output && strstr(output, "\"ok\": false") &&
          !strstr(output, "SCRIPT_SCOPE_PRIVATE_EVIDENCE"));
   cJSON_Delete(reply);
   free(result);
   assert(agent_tools_permission_check("read_file", NULL) == AGENT_TOOL_PERMISSION_ROLE);
   agent_tools_denied_set(NULL, 0);
   result = tool_execute_script("python", body, 5, directory, NULL);
   reply = result ? cJSON_Parse(result) : NULL;
   assert(reply && cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0);
   output = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "stdout"));
   assert(output && strstr(output, "\"ok\": true") &&
          strstr(output, "SCRIPT_SCOPE_PRIVATE_EVIDENCE"));
   cJSON_Delete(reply);
   free(result);
   unlink(path);
   rmdir(directory);
   agent_tools_scope_restore(&previous);
   puts("  PASS: script RPC retains the caller's denied-tool ceiling");
}
