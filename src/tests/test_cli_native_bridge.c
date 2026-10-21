/* Real CLI marshalling + real HTTP transport, no operator endpoint or proxy
 * route fallback. This is the production cli_v1_forward entry, not a hand-built
 * MCP request sent instead of the registered CLI. */
#include "cli_client.h"
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
int main(void)
{
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
