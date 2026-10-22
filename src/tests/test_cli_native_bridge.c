/* Real CLI marshalling + real HTTP transport, no operator endpoint or proxy
 * route fallback. This is the production cli_v1_forward entry, not a hand-built
 * MCP request sent instead of the registered CLI. */
#include "cli_client.h"
#include "cli_proxy.h"
#include <sys/wait.h>
#include <stdatomic.h>
#include "server_native_cli.h"
#include "cli_v1_routes_internal.h"
#include "cJSON.h"
#include <aimee/core/connection/socket.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int listener;
static void *serve(void *unused)
{
   (void)unused;
   for (int call = 0; call < 4; call++)
   {
      assert(aimee_core_socket_wait_readable(listener, 5000) == 1);
      int fd = aimee_core_socket_accept(listener);
      assert(fd >= 0);
      char raw[8192];
      size_t used = 0, expected = 0;
      for (;;)
      {
         long got = aimee_core_socket_read(fd, raw + used, sizeof raw - used - 1);
         assert(got > 0);
         used += (size_t)got;
         raw[used] = 0;
         char *end = strstr(raw, "\r\n\r\n");
         if (end && !expected)
         {
            const char *length = strstr(raw, "Content-Length:");
            assert(length);
            expected = (size_t)(end + 4 - raw) + strtoul(length + 15, NULL, 10);
         }
         if (expected && used >= expected)
            break;
      }
      if (call == 0)
      {
         assert(!strncmp(raw, "GET /v1/cli/manifest HTTP/1.1\r\n",
                         strlen("GET /v1/cli/manifest HTTP/1.1\r\n")));
         assert(strstr(raw, "Authorization: Bearer exact-child-grant\r\n"));
         const char *manifest = "{\"manifest_version\":1,\"marshal\":[]}";
         char response[512];
         int n =
             snprintf(response, sizeof response,
                      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                      "Connection: close\r\n\r\n%s",
                      strlen(manifest), manifest);
         assert(!aimee_core_socket_write_all(fd, response, (size_t)n));
         aimee_core_socket_close(fd);
         continue;
      }
      assert(!strncmp(raw, "POST /v1/native/cli HTTP/1.1\r\n",
                      strlen("POST /v1/native/cli HTTP/1.1\r\n")));
      assert(strstr(raw, "Authorization: Bearer exact-child-grant\r\n"));
      cJSON *request = cJSON_Parse(strstr(raw, "\r\n\r\n") + 4);
      assert(request);
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "method")),
                     "index.investigate"));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "query")),
                     "actual registered CLI query"));
      const char *nonce = cJSON_GetStringValue(cJSON_GetObjectItem(request, "_cli_request_id"));
      assert(nonce && strlen(nonce) == 32 && strspn(nonce, "0123456789abcdef") == 32);
      char tool[128], id[33];
      cJSON *arguments = NULL;
      assert(!server_native_cli_project(request, tool, &arguments, id));
      assert(!strcmp(tool, "index_investigate"));
      assert(!cJSON_GetObjectItem(arguments, "protocol_version") &&
             !cJSON_GetObjectItem(arguments, "cwd") && !cJSON_GetObjectItem(arguments, "method"));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(arguments, "query")),
                     "actual registered CLI query"));
      cJSON_Delete(arguments);
      cJSON_Delete(request);
      const char *body = call == 3   ? "{\"isError\":false}"
                         : call == 2 ? "{\"content\":[{\"type\":\"text\",\"text\":\"actual owned "
                                       "refusal\"}],\"isError\":true}"
                                     : "{\"content\":[{\"type\":\"text\",\"text\":\"actual owned "
                                       "index result\"}],\"isError\":false}";
      char response[512];
      int size =
          snprintf(response, sizeof response,
                   "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                   "Connection: close\r\n\r\n%s",
                   strlen(body), body);
      assert(!aimee_core_socket_write_all(fd, response, (size_t)size));
      aimee_core_socket_close(fd);
   }
   return NULL;
}
static atomic_int strict_calls, strict_error;
static int strict_route(const char *method, const char *path, void *context)
{
   (void)context;
   return (!strcmp(method, "GET") && !strcmp(path, "/v1/cli/manifest")) ||
          (!strcmp(method, "POST") && !strcmp(path, "/v1/native/cli"));
}
static int strict_authorize(const char *method, const char *path, const char *token, void *context)
{
   return strict_route(method, path, context) && !strcmp(token, "exact-child-grant");
}
static int strict_forward(const char *method, const char *path, const char *headers,
                          const void *body, unsigned long length, const char *session,
                          const cli_proxy_io_t *io, void *context)
{
   (void)headers;
   (void)context;
   assert(!strcmp(session, "native-child-session"));
   assert(!strcmp(io->local_credential, "exact-child-grant"));
   const char *result;
   if (!strcmp(method, "GET"))
   {
      assert(!strcmp(path, "/v1/cli/manifest") && !length);
      result = "{\"manifest_version\":1,\"marshal\":[]}";
   }
   else
   {
      assert(!strcmp(path, "/v1/native/cli"));
      cJSON *request = cJSON_ParseWithLength(body, length);
      char tool[128], nonce[33];
      cJSON *arguments = NULL;
      assert(!server_native_cli_project(request, tool, &arguments, nonce));
      assert(!strcmp(tool, "index_investigate"));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(arguments, "query")),
                     "whole real CLI via strict proxy"));
      cJSON_Delete(arguments);
      cJSON_Delete(request);
      result =
          atomic_load(&strict_error)
              ? "{\"content\":[{\"type\":\"text\",\"text\":\"owned refusal\"}],\"isError\":true}"
              : "{\"content\":[{\"type\":\"text\",\"text\":\"owned result\"}],\"isError\":false}";
   }
   atomic_fetch_add(&strict_calls, 1);
   char response[1024];
   int n = snprintf(response, sizeof response,
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                    "Connection: close\r\n\r\n%s",
                    strlen(result), result);
   return io->write_response(response, (unsigned long)n, io->context);
}
static void strict_binary_entry(const char *binary)
{
   unsigned port = 0;
   cli_proxy_t *proxy =
       cli_proxy_start_authorized(0, "master-not-child", "native-child-session", &port,
                                  strict_route, strict_authorize, strict_forward, NULL);
   assert(proxy && port);
   char endpoint[64];
   snprintf(endpoint, sizeof endpoint, "http://127.0.0.1:%u", port);
   for (int error = 0; error < 2; error++)
   {
      atomic_store(&strict_error, error);
      pid_t pid = fork();
      assert(pid >= 0);
      if (!pid)
      {
         setenv("AIMEE_NATIVE_CLI", "1", 1);
         setenv("AIMEE_API_ENDPOINT", endpoint, 1);
         setenv("AIMEE_API_BEARER", "exact-child-grant", 1);
         execl(binary, binary, "--json", "index", "investigate", "whole real CLI via strict proxy",
               (char *)NULL);
         _exit(127);
      }
      int status;
      assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
      assert(WEXITSTATUS(status) == error);
   }
   cli_proxy_stop(proxy);
   assert(atomic_load(&strict_calls) == 4); /* both real GET + POST round trips */
   puts("whole thin-client main through actual strict proxy: ok");
}
int main(int argc, char **argv)
{
   if (argc == 2)
   {
      strict_binary_entry(argv[1]);
      return 0;
   }
   char home[] = "/var/tmp/aimee-cli-child-home-XXXXXX";
   assert(mkdtemp(home));
   setenv("AIMEE_HOME", home, 1);
   unsigned port;
   listener = aimee_core_socket_listen_loopback(0, &port);
   assert(listener >= 0);
   char endpoint[64];
   snprintf(endpoint, sizeof endpoint, "http://127.0.0.1:%u", port);
   setenv("AIMEE_API_ENDPOINT", endpoint, 1);
   setenv("AIMEE_API_BEARER", "exact-child-grant", 1);
   setenv("AIMEE_NATIVE_CLI", "1", 1);
   setenv("AIMEE_SERVER_TOKEN", "global-token-must-not-be-used", 1);
   pthread_t server;
   assert(!pthread_create(&server, NULL, serve, NULL));
   const cli_v1_route_t route = {.method = "index.investigate"};
   char *args[] = {"actual registered CLI query"};
   assert(cli_v1_forward(NULL, &route, 1, NULL, NULL, 1, args) == 0);
   /* A real tool refusal remains failure even when MCP HTTP succeeded. */
   assert(cli_v1_forward(NULL, &route, 1, NULL, NULL, 1, args) == 1);
   assert(cli_v1_forward(NULL, &route, 1, NULL, NULL, 1, args) == 1);
   pthread_join(server, NULL);
   aimee_core_socket_close(listener);
   unsetenv("AIMEE_API_BEARER");
   assert(!cli_v1_client_bearer()); /* no global token even on first manifest lookup */
   assert(cli_v1_forward(NULL, &route, 1, NULL, NULL, 1, args) == 1);
   setenv("AIMEE_API_BEARER", "exact-child-grant", 1);
   unsetenv("AIMEE_API_ENDPOINT");
   assert(!cli_v1_client_endpoint());
   assert(cli_v1_forward(NULL, &route, 1, NULL, NULL, 1, args) == 1);
   assert(!rmdir(home)); /* Native mode never reads/writes global manifest cache. */
   puts("registered CLI native child transport: ok");
}
