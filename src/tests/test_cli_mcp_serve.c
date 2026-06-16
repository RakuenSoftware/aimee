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

const char *platform_home_dir(void)
{
   return NULL;
}

/* Stub for the iter-50 aimee_home() resolver. Returning NULL keeps the
 * test's "no real config dir" behaviour, so client_session_id and
 * friends short-circuit before they hit syscalls. */
const char *aimee_home(void)
{
   return NULL;
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

/* Reverse-channel helpers live in cli_workspace_serve.c, which this unit test
 * does not link; stub them so cli_mcp_serve.o resolves. */
int cli_workspace_reverse_channel_start(void)
{
   return 0;
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
   return 0;
}
char *cli_v1_client_endpoint(void)
{
   return NULL;
}
char *cli_v1_client_bearer(void)
{
   return NULL;
}
const char *cli_v1_route_for_method(const char *method, const char **verb_out)
{
   (void)method;
   (void)verb_out;
   return NULL;
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
   return NULL;
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

      snprintf(g_last_mcp_call_cwd, sizeof(g_last_mcp_call_cwd), "%s",
               cJSON_IsString(jcwd) ? jcwd->valuestring : "");
      snprintf(g_last_mcp_call_arg_cwd, sizeof(g_last_mcp_call_arg_cwd), "%s",
               cJSON_IsString(jarg_cwd) ? jarg_cwd->valuestring : "");

      /* Simulate an error response for "fail_tool" */
      if (strcmp(tool, "fail_tool") == 0)
      {
         cJSON *resp = cJSON_CreateObject();
         cJSON_AddStringToObject(resp, "status", "error");
         cJSON_AddStringToObject(resp, "message", "tool execution failed");
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
   cJSON *session = cJSON_GetObjectItemCaseSensitive(cfg, "sessionId");
   assert(cJSON_IsString(version));
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

   cJSON *instructions = cJSON_GetObjectItemCaseSensitive(result, "instructions");
   assert(cJSON_IsString(instructions));
   assert(strstr(instructions->valuestring, "get_help") != NULL);
   assert(strstr(instructions->valuestring, "spawn_agent") != NULL);
   assert(strstr(instructions->valuestring, "delegate tool") != NULL);

   cJSON_Delete(resp);
   cJSON_Delete(req);
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

   cJSON_Delete(resp);
   cJSON_Delete(req);
}

static void test_tools_call_success(void)
{
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

   cJSON_Delete(resp);
   cJSON_Delete(req);
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
   test_tools_list();
   test_tools_call_success();
   test_tools_call_server_error();
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
