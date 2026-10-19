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
#include "agent_config.h"
#include "agent_source_authority.h"
#include "request_context.h"
#include "server.h"
#include "util.h"
#include "modules/workspace/workspace_provider.h"

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
   result = tool_execute_script(
       "python",
       "import json, aimee_tools\n"
       "print(json.dumps(aimee_tools.call('read_file', {'path': 'evidence.txt'})))\n",
       5, directory, NULL);
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

typedef struct
{
   const char *directory;
   atomic_int cancel;
   int seen, matches;
} script_context_fixture_t;
static script_context_fixture_t *script_context_fixture;

static int script_context_read(const workspace_provider_t *provider, const char *path, char **out,
                               size_t *length)
{
   script_context_fixture_t *fixture = script_context_fixture;
   const request_context_t *request = request_context_get();
   agent_request_creds_t credentials;
   agent_request_creds_snapshot(&credentials);
   agent_source_authority_snapshot_t source = {0};
   int captured = agent_source_authority_tls_capture_checked(&source);
   const char *cwd = run_cmd_get_cwd();
   fixture->seen++;
   fixture->matches =
       !captured && source.active && source.authority == 1 && source.paths &&
       !strcmp(source.paths, "evidence.txt") && !strcmp(source.root, fixture->directory) &&
       request && !strcmp(request->principal, "script-context-fixture") &&
       request->model_execution == 1 && request->capabilities == CAPS_AUTHENTICATED && cwd &&
       !strcmp(cwd, fixture->directory) && !strcmp(session_id(), "script-context-session") &&
       !strcmp(credentials.vault_principal, "webuser:script-context-fixture") &&
       !credentials.codex_token[0] && !credentials.api_key[0] && !credentials.claude_token[0] &&
       workspace_provider_active() == provider && agent_get_request_cancel() == &fixture->cancel;
   agent_source_authority_snapshot_destroy(&source);
   const workspace_provider_t *shared = workspace_provider_shared();
   int result = shared->read_all(shared, path, out, length);
   atomic_store(&fixture->cancel, 1); /* The next RPC call must observe the host's cancellation. */
   return result;
}

void test_script_rpc_keeps_host_context(void)
{
   request_context_snapshot_t previous_request;
   request_context_snapshot(&previous_request);
   agent_request_creds_t previous_credentials;
   agent_request_creds_snapshot(&previous_credentials);
   agent_source_authority_snapshot_t previous_source;
   int captured = agent_source_authority_tls_capture_checked(&previous_source);
   assert(!captured);
   const workspace_provider_t *previous_workspace = workspace_provider_active();
   atomic_int *previous_cancel = agent_get_request_cancel();
   char previous_session[128] = "";
   if (session_id_override_active())
      snprintf(previous_session, sizeof previous_session, "%s", session_id());
   char directory[MAX_PATH_LEN], path[MAX_PATH_LEN];
   snprintf(directory, sizeof directory, "%s/arc.XXXXXX", platform_tmpdir());
   char *created = platform_mkdtemp(directory);
   assert(created);
   snprintf(path, sizeof path, "%s/evidence.txt", directory);
   FILE *file = fopen(path, "w");
   assert(file);
   int written = fputs("SCRIPT_CONTEXT_EVIDENCE", file), closed = fclose(file);
   assert(written >= 0 && !closed);
   script_context_fixture_t fixture = {.directory = directory};
   atomic_init(&fixture.cancel, 0);
   script_context_fixture = &fixture;
   workspace_provider_t workspace = *workspace_provider_shared();
   workspace.read_all = script_context_read;
   workspace_provider_set_active(&workspace);
   request_context_t owner = {.capabilities = CAPS_ALL};
   snprintf(owner.principal, sizeof owner.principal, "%s", "script-context-fixture");
   snprintf(owner.request_id, sizeof owner.request_id, "%s", "script-context-request");
   request_context_set(&owner);
   agent_set_request_vault_principal("webuser:script-context-fixture");
   agent_set_request_codex_creds("fixture-only-parent-model-token", "fixture-account");
   session_id_set_override("script-context-session");
   agent_source_authority_tls_set(1, directory, "evidence.txt");
   agent_set_request_cancel(&fixture.cancel);
   char *result =
       tool_execute_script("python",
                           "import json, aimee_tools\n"
                           "first = aimee_tools.call('read_file', {'path': 'evidence.txt'})\n"
                           "second = aimee_tools.call('read_file', {'path': 'evidence.txt'})\n"
                           "print(json.dumps([first, second]))\n",
                           5, directory, NULL);
   cJSON *reply = result ? cJSON_Parse(result) : NULL;
   assert(reply && cJSON_GetObjectItemCaseSensitive(reply, "exit_code")->valueint == 0);
   const char *output = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "stdout"));
   assert(output && strstr(output, "SCRIPT_CONTEXT_EVIDENCE") && strstr(output, "cancelled"));
   assert(fixture.seen == 1 && fixture.matches && atomic_load(&fixture.cancel));
   const request_context_t *unchanged = request_context_get();
   assert(unchanged && !unchanged->model_execution && unchanged->capabilities == CAPS_ALL);
   cJSON_Delete(reply);
   free(result);
   agent_set_request_cancel(previous_cancel);
   workspace_provider_set_active(previous_workspace);
   script_context_fixture = NULL;
   request_context_restore(&previous_request);
   request_context_snapshot_clear(&previous_request);
   agent_request_creds_restore(&previous_credentials);
   agent_source_authority_tls_restore(&previous_source);
   if (previous_session[0])
      session_id_set_override(previous_session);
   else
      session_id_clear_override();
   unlink(path);
   rmdir(directory);
   puts("  PASS: script RPC retains cwd/provider/identity/source/cancellation without model "
        "credentials");
}
