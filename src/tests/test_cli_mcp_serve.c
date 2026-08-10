#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cJSON.h"
#include "cli_client.h"
#include "server.h"

static char g_last_mcp_call_cwd[4096];
static char g_last_mcp_call_arg_cwd[4096];
static char g_last_mcp_call_arg_project[4096];
static char g_last_mcp_call_arg_workspace[4096];
static char g_last_mcp_call_tool[128];
static int g_last_mcp_call_paths_count;
static int g_reverse_channel_starts;
static int g_reverse_channel_syncs;
static int g_reverse_channel_sync_rc;
static int g_remote_active;
static int g_remote_http_failures;
static int g_remote_http_calls;
static const char *g_git_root_to_return;
static int g_index_ensure_calls;
static int g_index_ensure_before_worktree;
static char g_index_ensure_root[4096];

const char *platform_home_dir(void)
{
   return NULL;
}

/* Stub for the iter-50 aimee_home() resolver. Returning NULL keeps the
 * test's "no real config dir" behaviour, so client_session_id and
 * friends short-circuit before they hit syscalls. */
/* NULL by default (the original behaviour: no real config dir, so
 * client_session_id and friends short-circuit before any syscall). The
 * session-id tests point it at a temp dir so the session-ppid file is real. */
static const char *g_aimee_home;

const char *aimee_home(void)
{
   return g_aimee_home;
}

const char *cli_ensure_server(void)
{
   static char socket_path[64];

   if (socket_path[0] == '\0')
      snprintf(socket_path, sizeof(socket_path), "/tmp/aimee-test-%d.sock", (int)getpid());
   return socket_path;
}

const char *cli_ensure_server_for_method(const char *method)
{
   (void)method;
   return cli_ensure_server();
}

/* Per-session worktree bootstrap. The real one shells out to git; here it is
 * stubbed so the test can drive both outcomes (isolated / not applicable) and
 * assert what `initialize` does with each. g_worktree_to_return == NULL means
 * "not applicable" (isolation off, already inside a worktree, not a repo). */
static const char *g_worktree_to_return;
static char g_worktree_sid[128];
static int g_worktree_ensure_calls;

int client_session_worktree_ensure(const char *sid, char *out, size_t cap)
{
   g_worktree_ensure_calls++;
   snprintf(g_worktree_sid, sizeof(g_worktree_sid), "%s", sid ? sid : "");
   if (!g_worktree_to_return)
   {
      if (out && cap)
         out[0] = '\0';
      return -1;
   }
   snprintf(out, cap, "%s", g_worktree_to_return);
   return 0;
}

static int g_ppid = 4242;

int platform_getppid(void)
{
   return g_ppid;
}

/* Reverse-channel helpers live in cli_workspace_serve.c, which this unit test
 * does not link; stub them so cli_mcp_serve.o resolves. */
int cli_workspace_reverse_channel_start(void)
{
   g_reverse_channel_starts++;
   return 0;
}
int cli_workspace_reverse_channel_sync(void)
{
   g_reverse_channel_syncs++;
   return g_reverse_channel_sync_rc;
}
void cli_workspace_reverse_channel_stop(void)
{
}

/* Remote-endpoint accessors used by server_request's remote-routing branch.
 * This test drives the local-socket path (cli_ensure_server returns a socket),
 * so cli_v1_has_remote_endpoint() returns 0 and the rest are never called;
 * stub them so cli_mcp_serve.o links standalone. */
int cli_v1_has_remote_endpoint(void)
{
   return g_remote_active;
}
int cli_v1_remote_endpoint_is_network(void)
{
   return g_remote_active;
}
int safe_exec_capture(const char *const argv[], char **out_buf, size_t max_out)
{
   (void)argv;
   (void)max_out;
   if (out_buf)
      *out_buf = g_git_root_to_return ? strdup(g_git_root_to_return) : NULL;
   return g_git_root_to_return ? 0 : 1;
}
int cli_index_ensure_remote(const char *root)
{
   g_index_ensure_calls++;
   g_index_ensure_before_worktree = (g_worktree_ensure_calls == 0);
   snprintf(g_index_ensure_root, sizeof(g_index_ensure_root), "%s", root ? root : "");
   return 0;
}
char *cli_v1_client_endpoint(void)
{
   return strdup("https://aimee.test:8743");
}
char *cli_v1_client_bearer(void)
{
   return strdup("test-bearer");
}
const char *cli_v1_route_for_method(const char *method, const char **verb_out)
{
   if (verb_out)
      *verb_out = strcmp(method, "mcp.tools_list") == 0 ? "GET" : "POST";
   return strcmp(method, "mcp.tools_list") == 0 ? "/v1/mcp/tools_list" : "/v1/mcp/call";
}
cJSON *cli_http_request(const char *endpoint, const char *method, const char *path,
                        const char *body_json, const char *bearer, int timeout_ms, int *http_status)
{
   (void)endpoint;
   (void)method;
   (void)path;
   (void)body_json;
   (void)bearer;
   (void)timeout_ms;
   if (http_status)
      *http_status = 0;
   g_remote_http_calls++;
   if (g_remote_http_calls <= g_remote_http_failures)
      return NULL;
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddArrayToObject(resp, "tools");
   return resp;
}

int cli_connect(cli_conn_t *conn, const char *socket_path)
{
   (void)socket_path;
   conn->fd = 1;
   return 0;
}

int cli_authenticate(cli_conn_t *conn)
{
   (void)conn;
   return 0;
}

void cli_close(cli_conn_t *conn)
{
   if (conn)
      conn->fd = -1;
}

const char *session_id(void)
{
   return "test-session";
}

cJSON *mcp_build_tools_list(void)
{
   return cJSON_CreateArray();
}

static cJSON *stub_git_content(const char *name)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", name);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

cJSON *handle_git_status(cJSON *args)
{
   (void)args;
   return stub_git_content("git_status");
}
cJSON *handle_git_commit(cJSON *args)
{
   (void)args;
   return stub_git_content("git_commit");
}
cJSON *handle_git_push(cJSON *args)
{
   (void)args;
   return stub_git_content("git_push");
}
cJSON *handle_git_branch(cJSON *args)
{
   (void)args;
   return stub_git_content("git_branch");
}
cJSON *handle_git_log(cJSON *args)
{
   (void)args;
   return stub_git_content("git_log");
}
cJSON *handle_git_diff_summary(cJSON *args)
{
   (void)args;
   return stub_git_content("git_diff_summary");
}
cJSON *handle_git_pr(cJSON *args)
{
   (void)args;
   return stub_git_content("git_pr");
}
cJSON *handle_git_verify(server_ctx_t *ctx, cJSON *args, const char *session_id)
{
   (void)ctx;
   (void)args;
   (void)session_id;
   return stub_git_content("git_verify");
}
cJSON *handle_git_pull(cJSON *args)
{
   (void)args;
   return stub_git_content("git_pull");
}
cJSON *handle_git_clone(cJSON *args)
{
   (void)args;
   return stub_git_content("git_clone");
}
cJSON *handle_git_stash(cJSON *args)
{
   (void)args;
   return stub_git_content("git_stash");
}
cJSON *handle_git_tag(cJSON *args)
{
   (void)args;
   return stub_git_content("git_tag");
}
cJSON *handle_git_fetch(cJSON *args)
{
   (void)args;
   return stub_git_content("git_fetch");
}
cJSON *handle_git_reset(cJSON *args)
{
   (void)args;
   return stub_git_content("git_reset");
}
cJSON *handle_git_restore(cJSON *args)
{
   (void)args;
   return stub_git_content("git_restore");
}
cJSON *handle_git_issue(cJSON *args)
{
   (void)args;
   return stub_git_content("git_issue");
}
cJSON *handle_git_merge(cJSON *args)
{
   (void)args;
   return stub_git_content("git_merge");
}
cJSON *handle_git_rebase(cJSON *args)
{
   (void)args;
   return stub_git_content("git_rebase");
}
cJSON *handle_git_cherry_pick(cJSON *args)
{
   (void)args;
   return stub_git_content("git_cherry_pick");
}
cJSON *handle_git_revert(cJSON *args)
{
   (void)args;
   return stub_git_content("git_revert");
}
cJSON *handle_git_sync(cJSON *args)
{
   (void)args;
   return stub_git_content("git_sync");
}
cJSON *handle_git_add(cJSON *args)
{
   (void)args;
   return stub_git_content("git_add");
}
cJSON *handle_git_switch(cJSON *args)
{
   (void)args;
   return stub_git_content("git_switch");
}
cJSON *handle_git_checkout(cJSON *args)
{
   (void)args;
   return stub_git_content("git_checkout");
}

static int g_tools_list_forbidden;

/* cli_mcp_serve now forwards over the co-located /v1 dispatch instead of the
 * legacy NDJSON socket, so the mock backend is cli_v1_dispatch_local (same
 * {method,...} request → canned dispatch response). */
cJSON *cli_v1_dispatch_local(cJSON *request, int timeout_ms)
{
   (void)timeout_ms;

   cJSON *method = cJSON_GetObjectItemCaseSensitive(request, "method");
   if (!cJSON_IsString(method))
      return NULL;

   if (strcmp(method->valuestring, "mcp.tools_list") == 0)
   {
      cJSON *resp = cJSON_CreateObject();
      if (g_tools_list_forbidden)
      {
         cJSON *error = cJSON_AddObjectToObject(resp, "error");
         cJSON_AddStringToObject(error, "message", "query token lacks catalog access");
         return resp;
      }
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON *tools = cJSON_CreateArray();
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "name", "search_memory");
      cJSON_AddItemToArray(tools, tool);
      cJSON_AddItemToObject(resp, "tools", tools);
      return resp;
   }

   if (strcmp(method->valuestring, "memory.list") == 0)
   {
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON *memories = cJSON_CreateArray();

      cJSON *memory = cJSON_CreateObject();
      cJSON *tier = cJSON_GetObjectItemCaseSensitive(request, "tier");
      cJSON *kind = cJSON_GetObjectItemCaseSensitive(request, "kind");
      cJSON_AddNumberToObject(memory, "id", 42);
      cJSON_AddStringToObject(memory, "tier", cJSON_IsString(tier) ? tier->valuestring : "L2");
      cJSON_AddStringToObject(memory, "kind", cJSON_IsString(kind) ? kind->valuestring : "fact");
      cJSON_AddStringToObject(memory, "key", "test-key");
      cJSON_AddStringToObject(memory, "content", "test content");
      cJSON_AddNumberToObject(memory, "confidence", 0.9);
      cJSON_AddNumberToObject(memory, "use_count", 3);
      cJSON_AddStringToObject(memory, "last_used_at", "2026-04-10T00:00:00Z");
      cJSON_AddStringToObject(memory, "created_at", "2026-04-10T00:00:00Z");
      cJSON_AddStringToObject(memory, "updated_at", "2026-04-10T00:00:00Z");
      cJSON_AddStringToObject(memory, "source_session", "test-session");
      cJSON_AddItemToArray(memories, memory);

      cJSON_AddItemToObject(resp, "memories", memories);
      return resp;
   }

   if (strcmp(method->valuestring, "memory.get") == 0)
   {
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddNumberToObject(resp, "id", 42);
      cJSON_AddStringToObject(resp, "tier", "L2");
      cJSON_AddStringToObject(resp, "kind", "fact");
      cJSON_AddStringToObject(resp, "key", "test-key");
      cJSON_AddStringToObject(resp, "content", "test content");
      cJSON_AddNumberToObject(resp, "confidence", 0.9);
      cJSON_AddNumberToObject(resp, "use_count", 3);
      cJSON_AddStringToObject(resp, "last_used_at", "2026-04-10T00:00:00Z");
      cJSON_AddStringToObject(resp, "created_at", "2026-04-10T00:00:00Z");
      cJSON_AddStringToObject(resp, "updated_at", "2026-04-10T00:00:00Z");
      cJSON_AddStringToObject(resp, "source_session", "test-session");
      return resp;
   }

   if (strcmp(method->valuestring, "mcp.call") == 0)
   {
      cJSON *jtool = cJSON_GetObjectItemCaseSensitive(request, "tool");
      const char *tool = cJSON_IsString(jtool) ? jtool->valuestring : "";
      cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(request, "cwd");
      cJSON *arguments = cJSON_GetObjectItemCaseSensitive(request, "arguments");
      cJSON *jarg_cwd =
          cJSON_IsObject(arguments) ? cJSON_GetObjectItemCaseSensitive(arguments, "cwd") : NULL;
      cJSON *jarg_project =
          cJSON_IsObject(arguments) ? cJSON_GetObjectItemCaseSensitive(arguments, "project") : NULL;
      cJSON *jarg_workspace = cJSON_IsObject(arguments)
                                  ? cJSON_GetObjectItemCaseSensitive(arguments, "workspace")
                                  : NULL;
      cJSON *jpaths =
          cJSON_IsObject(arguments) ? cJSON_GetObjectItemCaseSensitive(arguments, "paths") : NULL;

      snprintf(g_last_mcp_call_tool, sizeof(g_last_mcp_call_tool), "%s", tool);
      snprintf(g_last_mcp_call_cwd, sizeof(g_last_mcp_call_cwd), "%s",
               cJSON_IsString(jcwd) ? jcwd->valuestring : "");
      snprintf(g_last_mcp_call_arg_cwd, sizeof(g_last_mcp_call_arg_cwd), "%s",
               cJSON_IsString(jarg_cwd) ? jarg_cwd->valuestring : "");
      snprintf(g_last_mcp_call_arg_project, sizeof(g_last_mcp_call_arg_project), "%s",
               cJSON_IsString(jarg_project) ? jarg_project->valuestring : "");
      snprintf(g_last_mcp_call_arg_workspace, sizeof(g_last_mcp_call_arg_workspace), "%s",
               cJSON_IsString(jarg_workspace) ? jarg_workspace->valuestring : "");
      g_last_mcp_call_paths_count = cJSON_IsArray(jpaths) ? cJSON_GetArraySize(jpaths) : 0;

      /* Simulate an error response for "fail_tool" */
      if (strcmp(tool, "fail_tool") == 0)
      {
         cJSON *resp = cJSON_CreateObject();
         cJSON_AddStringToObject(resp, "status", "error");
         cJSON_AddStringToObject(resp, "message", "tool execution failed");
         return resp;
      }

      /* HTTP auth failures use the server's standard nested error envelope. */
      if (strcmp(tool, "permission_tool") == 0)
      {
         cJSON *resp = cJSON_CreateObject();
         cJSON *error = cJSON_AddObjectToObject(resp, "error");
         cJSON_AddStringToObject(error, "message", "write-tier grant required");
         cJSON_AddStringToObject(error, "type", "permission_error");
         return resp;
      }

      if (strcmp(tool, "session_status") == 0)
      {
         cJSON *resp = cJSON_CreateObject();
         cJSON_AddStringToObject(resp, "status", "ok");
         cJSON *content = cJSON_CreateArray();
         cJSON *block = cJSON_CreateObject();
         cJSON_AddStringToObject(block, "type", "text");
         cJSON_AddStringToObject(block, "text", "workflow session #7");
         cJSON_AddItemToArray(content, block);
         cJSON_AddItemToObject(resp, "content", content);
         cJSON *structured = cJSON_CreateObject();
         cJSON_AddNumberToObject(structured, "id", 7);
         cJSON_AddStringToObject(structured, "status", "active");
         cJSON_AddItemToObject(resp, "structuredContent", structured);
         return resp;
      }

      if (strcmp(tool, "get_help") == 0)
      {
         cJSON *arguments = cJSON_GetObjectItemCaseSensitive(request, "arguments");
         assert(cJSON_IsObject(arguments));

         cJSON *resp = cJSON_CreateObject();
         cJSON_AddStringToObject(resp, "status", "ok");
         cJSON *content = cJSON_CreateArray();
         cJSON *block = cJSON_CreateObject();
         cJSON_AddStringToObject(block, "type", "text");
         cJSON_AddStringToObject(block, "text", "Aimee delegate reference");
         cJSON_AddItemToArray(content, block);
         cJSON_AddItemToObject(resp, "content", content);
         return resp;
      }

      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON *content = cJSON_CreateArray();
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "text");
      cJSON_AddStringToObject(block, "text", "mock tool result");
      cJSON_AddItemToArray(content, block);
      cJSON_AddItemToObject(resp, "content", content);
      return resp;
   }

   return NULL;
}

#include "../cli_mcp_serve.c"

static cJSON *capture_response(cJSON *req)
{
   int pipefd[2];
   assert(pipe(pipefd) == 0);

   int saved_stdout = dup(STDOUT_FILENO);
   assert(saved_stdout >= 0);

   fflush(stdout);
   assert(dup2(pipefd[1], STDOUT_FILENO) >= 0);
   close(pipefd[1]);
   handle_request(req);
   fflush(stdout);
   assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
   close(saved_stdout);

   char buf[32768];
   size_t total = 0;
   while (total < sizeof(buf) - 1)
   {
      ssize_t n = read(pipefd[0], buf + total, sizeof(buf) - 1 - total);
      assert(n >= 0);
      if (n == 0)
         break;
      total += (size_t)n;
   }
   buf[total] = '\0';
   close(pipefd[0]);

   char *body;
   if (strncmp(buf, "Content-Length:", 15) == 0)
   {
      body = strstr(buf, "\r\n\r\n");
      assert(body != NULL);
      body += 4;
   }
   else
   {
      body = buf;
   }

   cJSON *resp = cJSON_Parse(body);
   assert(resp != NULL);
   return resp;
}

static void test_prompts_get_success(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 1);
   cJSON_AddStringToObject(req, "method", "prompts/get");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "search-and-summarize");
   cJSON *args = cJSON_AddObjectToObject(params, "arguments");
   cJSON_AddStringToObject(args, "query", "mcp");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));

   cJSON *description = cJSON_GetObjectItemCaseSensitive(result, "description");
   assert(cJSON_IsString(description));
   assert(strstr(description->valuestring, "Search aimee memories") != NULL);

   cJSON *messages = cJSON_GetObjectItemCaseSensitive(result, "messages");
   assert(cJSON_IsArray(messages));
   assert(cJSON_GetArraySize(messages) == 1);

   cJSON *message = cJSON_GetArrayItem(messages, 0);
   cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
   cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
   cJSON *text = cJSON_GetObjectItemCaseSensitive(content, "text");
   assert(cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0);
   assert(cJSON_IsString(text));
   assert(strstr(text->valuestring, "search_memory") != NULL);
   assert(strstr(text->valuestring, "\"mcp\"") != NULL);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_prompts_get_missing_argument(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 2);
   cJSON_AddStringToObject(req, "method", "prompts/get");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "delegate-task");
   cJSON_AddObjectToObject(params, "arguments");

   cJSON *resp = capture_response(req);
   cJSON *error = cJSON_GetObjectItemCaseSensitive(resp, "error");
   assert(cJSON_IsObject(error));

   cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
   cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
   assert(cJSON_IsNumber(code) && code->valueint == -32602);
   assert(cJSON_IsString(message));
   assert(strstr(message->valuestring, "Missing required arguments") != NULL);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_prompts_get_delegate_policy(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 2.5);
   cJSON_AddStringToObject(req, "method", "prompts/get");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "delegate-task");
   cJSON *args = cJSON_AddObjectToObject(params, "arguments");
   cJSON_AddStringToObject(args, "role", "review");
   cJSON_AddStringToObject(args, "prompt", "review the diff");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   cJSON *messages = cJSON_GetObjectItemCaseSensitive(result, "messages");
   assert(cJSON_IsArray(messages) && cJSON_GetArraySize(messages) == 1);
   cJSON *message = cJSON_GetArrayItem(messages, 0);
   cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
   cJSON *text = cJSON_GetObjectItemCaseSensitive(content, "text");
   assert(cJSON_IsString(text));
   assert(strstr(text->valuestring, "delegate") != NULL);
   assert(strstr(text->valuestring, "spawn_agent") != NULL);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_resources_templates_list(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 3);
   cJSON_AddStringToObject(req, "method", "resources/templates/list");
   cJSON_AddObjectToObject(req, "params");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   cJSON *templates = cJSON_GetObjectItemCaseSensitive(result, "resourceTemplates");
   assert(cJSON_IsArray(templates));
   assert(cJSON_GetArraySize(templates) == 2);

   cJSON *tier_template = cJSON_GetArrayItem(templates, 0);
   cJSON *uri_template = cJSON_GetObjectItemCaseSensitive(tier_template, "uriTemplate");
   assert(cJSON_IsString(uri_template));
   assert(strcmp(uri_template->valuestring, "aimee://memories/{tier}") == 0);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_resources_read_config(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 4);
   cJSON_AddStringToObject(req, "method", "resources/read");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "uri", "aimee://config");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   cJSON *contents = cJSON_GetObjectItemCaseSensitive(result, "contents");
   cJSON *item = cJSON_GetArrayItem(contents, 0);
   cJSON *mime = cJSON_GetObjectItemCaseSensitive(item, "mimeType");
   cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
   assert(cJSON_IsString(mime));
   assert(strcmp(mime->valuestring, "application/json") == 0);
   assert(cJSON_IsString(text));

   cJSON *cfg = cJSON_Parse(text->valuestring);
   assert(cfg != NULL);
   cJSON *version = cJSON_GetObjectItemCaseSensitive(cfg, "version");
   cJSON *mcp_version = cJSON_GetObjectItemCaseSensitive(cfg, "mcpVersion");
   cJSON *session = cJSON_GetObjectItemCaseSensitive(cfg, "sessionId");
   assert(cJSON_IsString(version));
   assert(strcmp(version->valuestring, AIMEE_VERSION) == 0);
   assert(cJSON_IsString(mcp_version));
   assert(strcmp(mcp_version->valuestring, AIMEE_VERSION) == 0);
   assert(cJSON_IsString(session));
   assert(strcmp(session->valuestring, "test-session") == 0);

   cJSON_Delete(cfg);
   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_resources_read_memory_list(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 5);
   cJSON_AddStringToObject(req, "method", "resources/read");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "uri", "aimee://memories/L2");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   cJSON *contents = cJSON_GetObjectItemCaseSensitive(result, "contents");
   cJSON *item = cJSON_GetArrayItem(contents, 0);
   cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
   assert(cJSON_IsString(text));

   cJSON *memories = cJSON_Parse(text->valuestring);
   assert(cJSON_IsArray(memories));
   assert(cJSON_GetArraySize(memories) == 1);

   cJSON *memory = cJSON_GetArrayItem(memories, 0);
   cJSON *tier = cJSON_GetObjectItemCaseSensitive(memory, "tier");
   assert(cJSON_IsString(tier));
   assert(strcmp(tier->valuestring, "L2") == 0);

   cJSON_Delete(memories);
   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_resources_read_memory_by_id(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 6);
   cJSON_AddStringToObject(req, "method", "resources/read");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "uri", "aimee://memory/42");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   cJSON *contents = cJSON_GetObjectItemCaseSensitive(result, "contents");
   cJSON *item = cJSON_GetArrayItem(contents, 0);
   cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
   assert(cJSON_IsString(text));

   cJSON *memory = cJSON_Parse(text->valuestring);
   assert(cJSON_IsObject(memory));
   cJSON *id = cJSON_GetObjectItemCaseSensitive(memory, "id");
   cJSON *key = cJSON_GetObjectItemCaseSensitive(memory, "key");
   assert(cJSON_IsNumber(id) && id->valueint == 42);
   assert(cJSON_IsString(key) && strcmp(key->valuestring, "test-key") == 0);

   cJSON_Delete(memory);
   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_resources_read_unknown_uri(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 7);
   cJSON_AddStringToObject(req, "method", "resources/read");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "uri", "aimee://unknown");

   cJSON *resp = capture_response(req);
   cJSON *error = cJSON_GetObjectItemCaseSensitive(resp, "error");
   cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
   assert(cJSON_IsNumber(code));
   assert(code->valueint == -32002);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_initialize(void)
{
   g_reverse_channel_starts = 0;
   /* No worktree available (isolation off / already isolated / not a repo):
    * initialize must still serve normally, with the plain instructions. */
   g_worktree_to_return = NULL;
   g_worktree_ensure_calls = 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 10);
   cJSON_AddStringToObject(req, "method", "initialize");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "protocolVersion", "2024-11-05");
   cJSON_AddObjectToObject(params, "capabilities");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));

   cJSON *version = cJSON_GetObjectItemCaseSensitive(result, "protocolVersion");
   assert(cJSON_IsString(version));
   assert(strcmp(version->valuestring, "2024-11-05") == 0);

   cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
   assert(cJSON_IsObject(caps));
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(caps, "tools")));
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(caps, "resources")));
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(caps, "prompts")));

   cJSON *info = cJSON_GetObjectItemCaseSensitive(result, "serverInfo");
   assert(cJSON_IsObject(info));
   cJSON *name = cJSON_GetObjectItemCaseSensitive(info, "name");
   assert(cJSON_IsString(name) && strcmp(name->valuestring, "aimee") == 0);
   cJSON *server_version = cJSON_GetObjectItemCaseSensitive(info, "version");
   assert(cJSON_IsString(server_version));
   assert(strcmp(server_version->valuestring, AIMEE_VERSION) == 0);

   cJSON *instructions = cJSON_GetObjectItemCaseSensitive(result, "instructions");
   assert(cJSON_IsString(instructions));
   assert(strstr(instructions->valuestring, "get_help") != NULL);
   assert(strstr(instructions->valuestring, "spawn_agent") != NULL);
   assert(strstr(instructions->valuestring, "delegate tool") != NULL);
   assert(strstr(instructions->valuestring, "isolated checkout") == NULL);

   /* LISTED TOOLS ARE DIRECTLY CALLABLE, AND THE TEXT MUST LEAD WITH THAT.
    *
    * Twice now agents have spent their tool budget on the discovery protocol
    * instead of the work: once at five of fourteen calls, and again on a
    * benchmark cell that made two find_tools, two describe_tool and two call_tool
    * calls and not one direct find_symbol -- with find_symbol advertised the
    * whole time. Pin the ordering, because the ordering is the behaviour. */
   {
      const char *ins = instructions->valuestring;
      assert(strstr(ins, "directly callable") != NULL);
      assert(strstr(ins, "Do not route a listed tool through call_tool") != NULL);
      /* Discovery must be framed as the exception, i.e. introduced only after the
       * direct-call rule, and explicitly for tools that are NOT listed. */
      assert(strstr(ins, "NOT listed") != NULL);
      assert(strstr(ins, "directly callable") < strstr(ins, "find_tools"));
      /* get_help must not be ordered before doing any work. */
      assert(strstr(ins, "before trying anything else") == NULL);
   }
   assert(g_worktree_ensure_calls == 1);
   assert(g_reverse_channel_starts == 0);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

/* An MCP-hosted session has no SessionStart hook, so `initialize` is where it
 * gets isolated: the proxy must ENTER the prepared worktree (it is the process
 * aimee's file/exec tools resolve paths in) and tell the caller where its work
 * will land. */
static void test_initialize_enters_session_worktree(void)
{
   char origin_cwd[4096];
   assert(getcwd(origin_cwd, sizeof(origin_cwd)));

   char wt[4096];
   const char *tmp = getenv("TMPDIR");
   snprintf(wt, sizeof(wt), "%s/aimee-mcp-init-wt-%d", (tmp && tmp[0]) ? tmp : "/tmp",
            (int)getpid());
   char mk[4200];
   snprintf(mk, sizeof(mk), "rm -rf '%s' && mkdir -p '%s'", wt, wt);
   assert(system(mk) == 0);

   g_worktree_to_return = wt;
   g_worktree_ensure_calls = 0;
   g_worktree_sid[0] = '\0';

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 11);
   cJSON_AddStringToObject(req, "method", "initialize");
   cJSON_AddObjectToObject(req, "params");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));

   /* It asked for a worktree, keyed by a non-empty session id. */
   assert(g_worktree_ensure_calls == 1);
   assert(g_worktree_sid[0] != '\0');

   /* It entered it — this process's cwd IS what the file/exec tools use. */
   char now[4096];
   assert(getcwd(now, sizeof(now)));
   char real_wt[4096], real_now[4096];
   assert(realpath(wt, real_wt) && realpath(now, real_now));
   assert(strcmp(real_wt, real_now) == 0);

   /* And it told the caller, without dropping the base instructions. */
   cJSON *instructions = cJSON_GetObjectItemCaseSensitive(result, "instructions");
   assert(cJSON_IsString(instructions));
   assert(strstr(instructions->valuestring, "get_help") != NULL);
   assert(strstr(instructions->valuestring, "isolated checkout") != NULL);
   assert(strstr(instructions->valuestring, wt) != NULL);

   cJSON_Delete(resp);
   cJSON_Delete(req);

   assert(chdir(origin_cwd) == 0);
   snprintf(mk, sizeof(mk), "rm -rf '%s'", wt);
   (void)system(mk);
   g_worktree_to_return = NULL;
}

/* The remote server cannot infer or scan a thin client's checkout. Initialize
 * must seed the canonical root before worktree isolation changes cwd to the
 * hidden session path; otherwise the first find_symbol has no active project. */
static void test_remote_initialize_bootstraps_canonical_index(void)
{
   g_remote_active = 1;
   g_git_root_to_return = "/work/rakuen-blog\n";
   g_index_ensure_calls = 0;
   g_index_ensure_before_worktree = 0;
   g_index_ensure_root[0] = '\0';
   g_worktree_to_return = NULL;
   g_worktree_ensure_calls = 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 12);
   cJSON_AddStringToObject(req, "method", "initialize");
   cJSON_AddObjectToObject(req, "params");

   cJSON *resp = capture_response(req);
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(resp, "result")));
   assert(g_index_ensure_calls == 1);
   assert(g_index_ensure_before_worktree == 1);
   assert(strcmp(g_index_ensure_root, "/work/rakuen-blog") == 0);
   assert(g_worktree_ensure_calls == 1);

   cJSON_Delete(resp);
   cJSON_Delete(req);
   g_remote_active = 0;
   g_git_root_to_return = NULL;
}

static void test_tools_list(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 11);
   cJSON_AddStringToObject(req, "method", "tools/list");
   cJSON_AddObjectToObject(req, "params");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(result, "tools")));
   assert(g_reverse_channel_starts == 0);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_remote_discovery_retries_are_safe(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "mcp.tools_list");
   g_remote_active = 1;
   g_remote_http_calls = 0;
   g_remote_http_failures = 2;

   cJSON *resp = server_request(req, DEFAULT_TIMEOUT_MS);
   assert(resp != NULL);
   assert(g_remote_http_calls == 3);
   cJSON_Delete(resp);
   cJSON_Delete(req);

   /* A POST can have executed before its response was lost, so it must never
    * be replayed by the bridge. */
   req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "mcp.call");
   g_remote_http_calls = 0;
   g_remote_http_failures = 2;
   resp = server_request(req, DEFAULT_TIMEOUT_MS);
   assert(resp == NULL);
   assert(g_remote_http_calls == 1);
   cJSON_Delete(req);

   g_remote_active = 0;
   g_remote_http_calls = 0;
   g_remote_http_failures = 0;
}

static void test_tools_list_preserves_server_error(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 11);
   cJSON_AddStringToObject(req, "method", "tools/list");
   cJSON_AddObjectToObject(req, "params");

   g_tools_list_forbidden = 1;
   cJSON *resp = capture_response(req);
   g_tools_list_forbidden = 0;

   cJSON *error = cJSON_GetObjectItemCaseSensitive(resp, "error");
   cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
   assert(cJSON_IsString(message));
   assert(strstr(message->valuestring, "Failed to list tools") != NULL);
   assert(strstr(message->valuestring, "query token lacks catalog access") != NULL);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_tools_call_success(void)
{
   g_reverse_channel_starts = 0;
   g_reverse_channel_syncs = 0;
   g_last_mcp_call_cwd[0] = '\0';
   g_last_mcp_call_arg_cwd[0] = '\0';

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 12);
   cJSON_AddStringToObject(req, "method", "tools/call");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "search_memory");
   cJSON *args = cJSON_AddObjectToObject(params, "arguments");
   cJSON_AddStringToObject(args, "query", "test");
   /* The caller supplies no identity override. The stdio transport must attach
    * its cwd both to the request envelope and the tool arguments. */
   assert(cJSON_GetObjectItemCaseSensitive(args, "cwd") == NULL);
   assert(cJSON_GetObjectItemCaseSensitive(args, "project") == NULL);
   assert(cJSON_GetObjectItemCaseSensitive(args, "workspace") == NULL);

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
   assert(cJSON_IsArray(content) && cJSON_GetArraySize(content) == 1);
   cJSON *block = cJSON_GetArrayItem(content, 0);
   cJSON *text = cJSON_GetObjectItemCaseSensitive(block, "text");
   assert(cJSON_IsString(text));
   assert(strcmp(text->valuestring, "mock tool result") == 0);

   char cwd[4096];
   assert(getcwd(cwd, sizeof(cwd)) != NULL);
   assert(strcmp(g_last_mcp_call_cwd, cwd) == 0);
   assert(strcmp(g_last_mcp_call_arg_cwd, cwd) == 0);
   assert(g_reverse_channel_starts == 1);
   assert(g_reverse_channel_syncs == 0); /* non-Git calls do not pay the sync gate */

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_git_call_refreshes_mirror_and_fails_closed(void)
{
   g_reverse_channel_syncs = 0;
   g_reverse_channel_sync_rc = 0;
   g_last_mcp_call_tool[0] = '\0';

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 12.125);
   cJSON_AddStringToObject(req, "method", "tools/call");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "git");
   cJSON_AddObjectToObject(params, "arguments");

   cJSON *resp = capture_response(req);
   assert(g_reverse_channel_syncs == 1);
   assert(strcmp(g_last_mcp_call_tool, "git") == 0);
   cJSON_Delete(resp);

   g_reverse_channel_sync_rc = -1;
   g_last_mcp_call_tool[0] = '\0';
   resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
   assert(g_reverse_channel_syncs == 2);
   assert(g_last_mcp_call_tool[0] == '\0');
   cJSON_Delete(resp);
   cJSON_Delete(req);
   g_reverse_channel_sync_rc = 0;
}

static void test_tools_call_cwd_is_transport_owned(void)
{
   g_last_mcp_call_arg_cwd[0] = '\0';
   g_last_mcp_call_arg_project[0] = '\0';
   g_last_mcp_call_arg_workspace[0] = '\0';

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 12.25);
   cJSON_AddStringToObject(req, "method", "tools/call");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "search_memory");
   cJSON *args = cJSON_AddObjectToObject(params, "arguments");
   cJSON_AddStringToObject(args, "query", "test");
   cJSON_AddStringToObject(args, "cwd", "/tmp/model-spoofed-checkout");
   cJSON_AddStringToObject(args, "project", "explicit-project");
   cJSON_AddStringToObject(args, "workspace", "explicit-workspace");

   cJSON *resp = capture_response(req);
   char cwd[4096];
   assert(getcwd(cwd, sizeof(cwd)) != NULL);
   assert(strcmp(g_last_mcp_call_arg_cwd, cwd) == 0);
   assert(strcmp(g_last_mcp_call_arg_project, "explicit-project") == 0);
   assert(strcmp(g_last_mcp_call_arg_workspace, "explicit-workspace") == 0);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

/* Installed MCP smoke: the canonical blast-preview name and path-only schema
 * survive the stdio proxy, which supplies cwd for the server's active-project
 * default. This deliberately omits project. */
static void test_tools_call_preview_blast_radius(void)
{
   g_last_mcp_call_tool[0] = '\0';
   g_last_mcp_call_cwd[0] = '\0';
   g_last_mcp_call_arg_cwd[0] = '\0';
   g_last_mcp_call_paths_count = 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 12.5);
   cJSON_AddStringToObject(req, "method", "tools/call");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "preview_blast_radius");
   cJSON *args = cJSON_AddObjectToObject(params, "arguments");
   cJSON *paths = cJSON_AddArrayToObject(args, "paths");
   cJSON_AddItemToArray(paths, cJSON_CreateString("src/server/server_mcp.c"));

   cJSON *resp = capture_response(req);
   assert(strcmp(g_last_mcp_call_tool, "preview_blast_radius") == 0);
   assert(g_last_mcp_call_paths_count == 1);
   char cwd[4096];
   assert(getcwd(cwd, sizeof(cwd)) != NULL);
   assert(strcmp(g_last_mcp_call_cwd, cwd) == 0);
   assert(strcmp(g_last_mcp_call_arg_cwd, cwd) == 0);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));

   cJSON_Delete(resp);
   cJSON_Delete(req);
   puts("  PASS: test_tools_call_preview_blast_radius");
}

static void test_tools_call_server_error(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 13);
   cJSON_AddStringToObject(req, "method", "tools/call");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "fail_tool");
   cJSON_AddObjectToObject(params, "arguments");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
   cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
   assert(cJSON_IsArray(content));
   cJSON *block = cJSON_GetArrayItem(content, 0);
   cJSON *text = cJSON_GetObjectItemCaseSensitive(block, "text");
   assert(cJSON_IsString(text));
   assert(strstr(text->valuestring, "tool execution failed") != NULL);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_tools_call_nested_http_error(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 13.25);
   cJSON_AddStringToObject(req, "method", "tools/call");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "permission_tool");
   cJSON_AddObjectToObject(params, "arguments");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
   cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
   cJSON *block = cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : NULL;
   cJSON *text = block ? cJSON_GetObjectItemCaseSensitive(block, "text") : NULL;
   assert(cJSON_IsString(text));
   assert(strstr(text->valuestring, "write-tier grant required") != NULL);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_tools_call_structured_content_passthrough(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 13.5);
   cJSON_AddStringToObject(req, "method", "tools/call");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "session_status");
   cJSON *args = cJSON_AddObjectToObject(params, "arguments");
   cJSON_AddNumberToObject(args, "id", 7);

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   cJSON *structured = cJSON_GetObjectItemCaseSensitive(result, "structuredContent");
   assert(cJSON_IsObject(structured));
   assert(cJSON_GetObjectItem(structured, "id")->valueint == 7);
   assert(strcmp(cJSON_GetObjectItem(structured, "status")->valuestring, "active") == 0);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_tools_call_get_help_without_arguments(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 13.75);
   cJSON_AddStringToObject(req, "method", "tools/call");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "get_help");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
   assert(cJSON_IsArray(content));
   cJSON *block = cJSON_GetArrayItem(content, 0);
   cJSON *text = cJSON_GetObjectItemCaseSensitive(block, "text");
   assert(cJSON_IsString(text));
   assert(strstr(text->valuestring, "Aimee delegate reference") != NULL);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_tools_call_get_help_with_empty_object_arguments(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 42);
   cJSON_AddStringToObject(req, "method", "tools/call");
   cJSON *params = cJSON_AddObjectToObject(req, "params");
   cJSON_AddStringToObject(params, "name", "get_help");
   cJSON_AddObjectToObject(params, "arguments"); /* explicit {} */

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
   assert(cJSON_IsArray(content));
   cJSON *block = cJSON_GetArrayItem(content, 0);
   cJSON *text = cJSON_GetObjectItemCaseSensitive(block, "text");
   assert(cJSON_IsString(text));
   assert(strstr(text->valuestring, "Aimee delegate reference") != NULL);
   /* No isError flag — transport must not close on empty-object args */
   cJSON *is_err = cJSON_GetObjectItemCaseSensitive(result, "isError");
   assert(!is_err || !cJSON_IsTrue(is_err));

   cJSON_Delete(resp);
   cJSON_Delete(req);
   puts("  PASS: test_tools_call_get_help_with_empty_object_arguments");
}

static void test_tools_call_missing_params(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 14);
   cJSON_AddStringToObject(req, "method", "tools/call");
   /* No params */

   cJSON *resp = capture_response(req);
   cJSON *error = cJSON_GetObjectItemCaseSensitive(resp, "error");
   assert(cJSON_IsObject(error));
   cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
   assert(cJSON_IsNumber(code) && code->valueint == -32602);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_prompts_list(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 15);
   cJSON_AddStringToObject(req, "method", "prompts/list");
   cJSON_AddObjectToObject(req, "params");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   cJSON *prompts = cJSON_GetObjectItemCaseSensitive(result, "prompts");
   assert(cJSON_IsArray(prompts));
   assert(cJSON_GetArraySize(prompts) == 3);

   /* Verify first prompt is search-and-summarize */
   cJSON *first = cJSON_GetArrayItem(prompts, 0);
   cJSON *pname = cJSON_GetObjectItemCaseSensitive(first, "name");
   assert(cJSON_IsString(pname));
   assert(strcmp(pname->valuestring, "search-and-summarize") == 0);

   cJSON *second = cJSON_GetArrayItem(prompts, 1);
   cJSON *desc = cJSON_GetObjectItemCaseSensitive(second, "description");
   assert(cJSON_IsString(desc));
   assert(strstr(desc->valuestring, "aimee") != NULL);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_resources_list(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 16);
   cJSON_AddStringToObject(req, "method", "resources/list");
   cJSON_AddObjectToObject(req, "params");

   cJSON *resp = capture_response(req);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   assert(cJSON_IsObject(result));
   cJSON *resources = cJSON_GetObjectItemCaseSensitive(result, "resources");
   assert(cJSON_IsArray(resources));
   /* 4 memory tiers (L0-L3) + aimee://facts + aimee://config = 6 */
   assert(cJSON_GetArraySize(resources) == 6);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_unknown_method(void)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", 17);
   cJSON_AddStringToObject(req, "method", "nonexistent/method");
   cJSON_AddObjectToObject(req, "params");

   cJSON *resp = capture_response(req);
   cJSON *error = cJSON_GetObjectItemCaseSensitive(resp, "error");
   assert(cJSON_IsObject(error));
   cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
   assert(cJSON_IsNumber(code) && code->valueint == -32601);

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_notification_no_response(void)
{
   /* Notifications have no id; handle_request must accept them silently */
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddStringToObject(req, "method", "notifications/initialized");
   /* No id field */

   int pipefd[2];
   assert(pipe(pipefd) == 0);
   int saved_stdout = dup(STDOUT_FILENO);
   assert(saved_stdout >= 0);
   fflush(stdout);
   assert(dup2(pipefd[1], STDOUT_FILENO) >= 0);
   close(pipefd[1]);
   handle_request(req);
   fflush(stdout);
   assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
   close(saved_stdout);

   /* Check no bytes written */
   int flags = fcntl(pipefd[0], F_GETFL, 0);
   assert(fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == 0);
   char buf[64];
   ssize_t n = read(pipefd[0], buf, sizeof(buf));
   close(pipefd[0]);
   assert(n <= 0);

   cJSON_Delete(req);
}

/* The session id keys the worktree, so how it is minted decides whether two
 * processes share a checkout or collide in one. Processes of one agent session
 * share a PPID and must converge on ONE id via session-ppid-<ppid>; a private
 * invented id would put this proxy in a different worktree from its own
 * session, and a bare ppid string would put two proxies under one host into the
 * SAME worktree, overwriting each other. */
static void test_session_id_is_shared_not_invented(void)
{
   char home[512];
   const char *tmp = getenv("TMPDIR");
   snprintf(home, sizeof(home), "%s/aimee-mcp-sid-%d", (tmp && tmp[0]) ? tmp : "/tmp",
            (int)getpid());
   char cmd[600];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", home, home);
   assert(system(cmd) == 0);
   g_aimee_home = home;
   /* No host-provided id: this is the path where the proxy must mint one. */
   unsetenv("AIMEE_SESSION_ID");

   /* First caller mints and publishes. */
   char first[64] = "";
   assert(client_session_id_ensure(first, sizeof(first)) == 0);
   assert(first[0]);

   char published[600];
   snprintf(published, sizeof(published), "%s/session-ppid-%d", home, g_ppid);
   struct stat st;
   assert(stat(published, &st) == 0); /* it PUBLISHED, so siblings can find it */

   /* A sibling process of the same session (same ppid) adopts that id rather
    * than minting a second one — same session, same worktree. */
   char second[64] = "";
   assert(client_session_id_ensure(second, sizeof(second)) == 0);
   assert(strcmp(first, second) == 0);

   /* A process under a DIFFERENT host gets a different id — two hosts must not
    * share one worktree. */
   g_ppid = 4243;
   char other[64] = "";
   assert(client_session_id_ensure(other, sizeof(other)) == 0);
   assert(strcmp(first, other) != 0);

   /* Orphaned (ppid <= 1) refuses rather than keying on session-ppid-1, which
    * every unrelated daemon on the box would share. */
   g_ppid = 1;
   char orphan[64] = "";
   assert(client_session_id_ensure(orphan, sizeof(orphan)) == -1);
   assert(orphan[0] == '\0');

   /* An explicit AIMEE_SESSION_ID always wins and needs no publishing — the
    * host has already decided which session this is. */
   setenv("AIMEE_SESSION_ID", "host-provided-id", 1);
   char from_env[64] = "";
   assert(client_session_id_ensure(from_env, sizeof(from_env)) == 0);
   assert(strcmp(from_env, "host-provided-id") == 0);

   g_ppid = 4242;
   g_aimee_home = NULL;
   setenv("AIMEE_SESSION_ID", "test-session", 1); /* restore for later cases */
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", home);
   (void)system(cmd);
   printf("  PASS: test_session_id_is_shared_not_invented\n");
}

int main(void)
{
   setenv("AIMEE_SESSION_ID", "test-session", 1);
   test_prompts_get_success();
   test_prompts_get_missing_argument();
   test_prompts_get_delegate_policy();
   test_resources_templates_list();
   test_resources_read_config();
   test_resources_read_memory_list();
   test_resources_read_memory_by_id();
   test_resources_read_unknown_uri();
   test_initialize();
   test_initialize_enters_session_worktree();
   test_remote_initialize_bootstraps_canonical_index();
   test_session_id_is_shared_not_invented();
   test_tools_list();
   test_tools_list_preserves_server_error();
   test_remote_discovery_retries_are_safe();
   test_tools_call_success();
   test_git_call_refreshes_mirror_and_fails_closed();
   test_tools_call_cwd_is_transport_owned();
   test_tools_call_preview_blast_radius();
   test_tools_call_server_error();
   test_tools_call_nested_http_error();
   test_tools_call_structured_content_passthrough();
   test_tools_call_get_help_without_arguments();
   test_tools_call_get_help_with_empty_object_arguments();
   test_tools_call_missing_params();
   test_prompts_list();
   test_resources_list();
   test_unknown_method();
   test_notification_no_response();
   puts("cli_mcp_serve: all tests passed");
   return 0;
}
