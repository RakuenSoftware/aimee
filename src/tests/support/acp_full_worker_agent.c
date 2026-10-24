/* Pure ACP protocol peer for the full-worker container fixture. No SDK/model.
 * Build statically inside the pinned Debian builder, then install only in the
 * task-owned sandbox image at /usr/local/bin/aimee-acp-fixture-agent. */
#include "cJSON.h"
#include <arpa/inet.h>
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

static cJSON *fixture_read(void)
{
   static char line[1024 * 1024];
   assert(fgets(line, sizeof line, stdin));
   assert(strchr(line, '\n'));
   const char *end = NULL;
   cJSON *frame = cJSON_ParseWithOpts(line, &end, 1);
   assert(frame && cJSON_IsObject(frame));
   return frame;
}
static void fixture_send(cJSON *frame)
{
   char *line = cJSON_PrintUnformatted(frame);
   assert(line && fputs(line, stdout) >= 0 && putchar('\n') == '\n' && !fflush(stdout));
   free(line);
   cJSON_Delete(frame);
}
static void fixture_reply(cJSON *request, const char *result)
{
   cJSON *frame = cJSON_CreateObject();
   assert(
       frame && cJSON_AddStringToObject(frame, "jsonrpc", "2.0") &&
       cJSON_AddItemToObject(frame, "id", cJSON_Duplicate(cJSON_GetObjectItem(request, "id"), 1)) &&
       cJSON_AddItemToObject(frame, "result", cJSON_Parse(result)));
   fixture_send(frame);
}
typedef struct
{
   pid_t pid;
   int input, output;
} fixture_mcp_t;
static fixture_mcp_t fixture_mcp_start(const cJSON *servers)
{
   assert(cJSON_IsArray(servers) && cJSON_GetArraySize(servers) == 1);
   const cJSON *spec = cJSON_GetArrayItem(servers, 0);
   const char *command = cJSON_GetStringValue(cJSON_GetObjectItem(spec, "command"));
   const cJSON *args = cJSON_GetObjectItem(spec, "args"), *env = cJSON_GetObjectItem(spec, "env");
   assert(command && command[0] == '/' && cJSON_IsArray(args) && cJSON_IsArray(env));
   int argc = cJSON_GetArraySize(args), envc = cJSON_GetArraySize(env);
   assert(argc >= 1 && argc < 16 && envc > 0 && envc < 64);
   char *arguments[18] = {(char *)command}, *environment[65] = {0};
   for (int i = 0; i < argc; i++)
   {
      arguments[i + 1] = cJSON_GetStringValue(cJSON_GetArrayItem(args, i));
      assert(arguments[i + 1]);
   }
   assert(!strcmp(arguments[1], "mcp-serve"));
   for (int i = 0; i < envc; i++)
   {
      const cJSON *entry = cJSON_GetArrayItem(env, i);
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "name")),
                 *value = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "value"));
      assert(name && name[0] && !strchr(name, '=') && value);
      size_t size = strlen(name) + strlen(value) + 2;
      assert(size < 16384 && (environment[i] = malloc(size)));
      snprintf(environment[i], size, "%s=%s", name, value);
   }
   int input[2], output[2];
   assert(!pipe(input) && !pipe(output));
   pid_t pid = fork();
   assert(pid >= 0);
   if (!pid)
   {
      close(input[1]);
      close(output[0]);
      assert(dup2(input[0], STDIN_FILENO) == STDIN_FILENO &&
             dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
      close(input[0]);
      close(output[1]);
      execve(command, arguments, environment);
      _exit(127);
   }
   close(input[0]);
   close(output[1]);
   for (int i = 0; i < envc; i++)
      free(environment[i]);
   return (fixture_mcp_t){.pid = pid, .input = input[1], .output = output[0]};
}
static cJSON *fixture_mcp_exchange(fixture_mcp_t *peer, cJSON *request)
{
   const cJSON *id = cJSON_GetObjectItem(request, "id");
   assert(cJSON_IsNumber(id));
   int expected = id->valueint;
   char *wire = cJSON_PrintUnformatted(request);
   assert(wire);
   size_t length = strlen(wire), sent = 0;
   while (sent < length + 1)
   {
      struct pollfd ready = {.fd = peer->input, .events = POLLOUT};
      assert(poll(&ready, 1, 15000) > 0);
      const char *part = sent == length ? "\n" : wire + sent;
      size_t count = sent == length ? 1 : length - sent;
      ssize_t n = write(peer->input, part, count);
      assert(n > 0);
      sent += (size_t)n;
   }
   free(wire);
   cJSON_Delete(request);
   for (int notification = 0; notification < 32; notification++)
   {
      char *line = malloc(1024 * 1024);
      assert(line);
      size_t used = 0;
      do
      {
         struct pollfd ready = {.fd = peer->output, .events = POLLIN};
         assert(poll(&ready, 1, 15000) > 0 && used + 1 < 1024 * 1024);
         assert(read(peer->output, line + used, 1) == 1);
      } while (line[used++] != '\n');
      line[used] = 0;
      cJSON *response = cJSON_Parse(line);
      free(line);
      assert(response);
      const cJSON *received = cJSON_GetObjectItem(response, "id");
      if (received)
      {
         assert(cJSON_IsNumber(received) && received->valueint == expected);
         return response;
      }
      const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(response, "method"));
      assert(method && !strncmp(method, "notifications/", 14));
      cJSON_Delete(response);
   }
   assert(!"MCP response missing");
   return NULL;
}
static void fixture_mcp_sequence(const cJSON *servers, const char *path)
{
   fixture_mcp_t peer = fixture_mcp_start(servers);
   cJSON *reply = fixture_mcp_exchange(
       &peer, cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"initialize\",\"params\":{"
                          "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{"
                          "\"name\":\"full-worker-fixture\",\"version\":\"1\"}}}"));
   assert(cJSON_IsObject(cJSON_GetObjectItem(reply, "result")) &&
          !cJSON_GetObjectItem(reply, "error"));
   cJSON_Delete(reply);
   reply = fixture_mcp_exchange(
       &peer, cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"tools/list\"}"));
   const cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(reply, "result"), "tools");
   assert(cJSON_IsArray(tools) && cJSON_GetArraySize(tools) > 0);
   int read = 0, bash = 0;
   const cJSON *tool;
   cJSON_ArrayForEach(tool, tools)
   {
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "name"));
      assert(name && cJSON_IsObject(cJSON_GetObjectItem(tool, "inputSchema")));
      read |= !strcmp(name, "read_file");
      bash |= !strcmp(name, "bash");
   }
   assert(read && bash);
   fprintf(stderr, "full-worker-agent: actual advertised MCP tools/list count=%d\n",
           cJSON_GetArraySize(tools));
   cJSON_Delete(reply);
   for (int step = 0; step < 2; step++)
   {
      cJSON *call = cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":101,\"method\":\"tools/"
                                "call\",\"params\":{\"arguments\":{}}}");
      cJSON_ReplaceItemInObject(call, "id", cJSON_CreateNumber(101 + step));
      cJSON *params = cJSON_GetObjectItem(call, "params"),
            *arguments = cJSON_GetObjectItem(params, "arguments");
      assert(cJSON_AddStringToObject(params, "name", step ? "bash" : "read_file"));
      if (step)
         assert(
             cJSON_AddStringToObject(arguments, "command",
                                     "/usr/local/bin/aimee --json tool call read_file --arguments "
                                     "'{\"path\":\"full-worker.txt\",\"raw\":true}'"));
      else
         assert(cJSON_AddStringToObject(arguments, "path", path) &&
                cJSON_AddBoolToObject(arguments, "raw", 1));
      reply = fixture_mcp_exchange(&peer, call);
      const cJSON *result = cJSON_GetObjectItem(reply, "result");
      assert(cJSON_IsObject(result) && !cJSON_GetObjectItem(reply, "error") &&
             !cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
      char *text = cJSON_PrintUnformatted(result);
      assert(text && strstr(text, "full worker owned write"));
      free(text);
      cJSON_Delete(reply);
   }
   close(peer.input);
   int status = 0, joined = 0;
   for (int i = 0; i < 500 && !joined; i++)
   {
      pid_t result = waitpid(peer.pid, &status, WNOHANG);
      assert(result >= 0);
      joined = result == peer.pid;
      if (!joined)
         usleep(10000);
   }
   assert(joined && WIFEXITED(status) && !WEXITSTATUS(status));
   close(peer.output);
   fputs("full-worker-agent: actual MCP read and Bash -> installed CLI read complete\n", stderr);
}

static int fixture_proxy_head(const char *url, const char *host)
{
   int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
   assert(fd >= 0);
   struct sockaddr_in endpoint = {
       .sin_family = AF_INET, .sin_port = htons(3129), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
   assert(!connect(fd, (struct sockaddr *)&endpoint, sizeof endpoint));
   char request[1024];
   int n = snprintf(request, sizeof request,
                    "HEAD %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", url, host);
   assert(n > 0 && (size_t)n < sizeof request);
   size_t sent = 0;
   while (sent < (size_t)n)
   {
      struct pollfd ready = {.fd = fd, .events = POLLOUT};
      assert(poll(&ready, 1, 15000) > 0);
      ssize_t count = send(fd, request + sent, (size_t)n - sent, MSG_NOSIGNAL);
      assert(count > 0);
      sent += (size_t)count;
   }
   char line[1024];
   size_t length = 0;
   do
   {
      struct pollfd ready = {.fd = fd, .events = POLLIN};
      assert(poll(&ready, 1, 15000) > 0 && length + 1 < sizeof line);
      assert(recv(fd, line + length, 1, 0) == 1);
   } while (line[length++] != '\n');
   line[length] = 0;
   int status = 0;
   assert(sscanf(line, "HTTP/%*u.%*u %d", &status) == 1 && status >= 100 && status <= 599);
   assert(!close(fd));
   return status;
}
static void fixture_package_proxy(void)
{
   const char *http = getenv("http_proxy"), *https = getenv("https_proxy");
   assert(http && https &&
          (!strcmp(http, "http://127.0.0.1:3129") || !strcmp(http, "http://127.0.0.1:3129/")) &&
          (!strcmp(https, "http://127.0.0.1:3129") || !strcmp(https, "http://127.0.0.1:3129/")));
   int status = fixture_proxy_head("http://deb.debian.org/debian/", "deb.debian.org");
   assert(status >= 200 && status < 400);
   assert(fixture_proxy_head("http://127.0.0.1/", "127.0.0.1") == 403);
   fputs("full-worker-agent: default package proxy allowed Debian HEAD and denied loopback\n",
         stderr);
}
int main(void)
{
   alarm(55);
   cJSON *request = fixture_read();
   const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(request, "method"));
   assert(method && !strcmp(method, "initialize"));
   fixture_reply(request, "{\"protocolVersion\":1,\"agentCapabilities\":{},\"authMethods\":[]}");
   cJSON_Delete(request);
   request = fixture_read();
   method = cJSON_GetStringValue(cJSON_GetObjectItem(request, "method"));
   assert(method && !strcmp(method, "session/new"));
   const char *cwd =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(request, "params"), "cwd"));
   char actual[4096];
   assert(cwd && getcwd(actual, sizeof actual) && !strcmp(actual, cwd));
   cJSON *servers = cJSON_Duplicate(
       cJSON_GetObjectItem(cJSON_GetObjectItem(request, "params"), "mcpServers"), 1);
   assert(servers);
   fixture_reply(request, "{\"sessionId\":\"full-worker-acp\"}");
   cJSON_Delete(request);
   request = fixture_read();
   method = cJSON_GetStringValue(cJSON_GetObjectItem(request, "method"));
   assert(method && !strcmp(method, "session/prompt"));
   fixture_package_proxy();
   char target[4096];
   assert(snprintf(target, sizeof target, "%s/full-worker.txt", actual) < (int)sizeof target);
   cJSON *call = cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":\"full-worker-write\",\"method\":\"fs/"
                             "write_text_file\",\"params\":{\"sessionId\":\"full-worker-acp\"}}");
   assert(call && cJSON_AddStringToObject(cJSON_GetObjectItem(call, "params"), "path", target) &&
          cJSON_AddStringToObject(cJSON_GetObjectItem(call, "params"), "content",
                                  "full worker owned write\n"));
   fixture_send(call);
   cJSON *receipt = fixture_read();
   assert(cJSON_IsObject(cJSON_GetObjectItem(receipt, "result")) &&
          !cJSON_GetObjectItem(receipt, "error"));
   cJSON_Delete(receipt);
   call = cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":\"full-worker-read\",\"method\":\"fs/"
                      "read_text_file\",\"params\":{\"sessionId\":\"full-worker-acp\"}}");
   assert(call && cJSON_AddStringToObject(cJSON_GetObjectItem(call, "params"), "path", target));
   fixture_send(call);
   receipt = fixture_read();
   const char *content =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(receipt, "result"), "content"));
   assert(content && !strcmp(content, "full worker owned write\n"));
   cJSON_Delete(receipt);
   fixture_mcp_sequence(servers, target);
   cJSON_Delete(servers);
   fixture_send(cJSON_Parse("{\"jsonrpc\":\"2.0\",\"method\":\"session/"
                            "update\",\"params\":{\"sessionId\":\"full-worker-acp\",\"update\":{"
                            "\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":"
                            "\"text\",\"text\":\"Full delegate worker ACP complete\"}}}}"));
   fixture_reply(request, "{\"stopReason\":\"end_turn\"}");
   cJSON_Delete(request);
   return 0;
}
