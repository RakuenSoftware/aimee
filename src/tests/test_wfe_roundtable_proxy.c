/* Verify the C -> Go roundtable boundary forwards the resolved saved default.
 * The Go service deliberately requires a named panel, so losing this field
 * makes an omitted MCP roundtable fail even though C resolved it successfully. */
#include <stdint.h>

#include "cJSON.h"
#include "config_accessors.h"
#include "platform_test_util.h"
#include "roundtable_preset.h"
#include "server.h"
#include "server/wfe_roundtable_proxy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_sent_ok;
static int g_sent_error;
static int g_sent_deadline;
static int g_sent_participant_failures;

static size_t framed_request_size(const char *request)
{
   const char *headers_end = strstr(request, "\r\n\r\n");
   if (!headers_end)
      return 0;
   const char *length = strstr(request, "\r\nContent-Length:");
   if (!length || length >= headers_end)
      return SIZE_MAX;
   length += strlen("\r\nContent-Length:");
   while (*length == ' ')
      ++length;
   char *end = NULL;
   unsigned long body_size = strtoul(length, &end, 10);
   if (end == length || end > headers_end)
      return SIZE_MAX;
   size_t header_size = (size_t)(headers_end + 4 - request);
   if (body_size > SIZE_MAX - header_size)
      return SIZE_MAX;
   return header_size + (size_t)body_size;
}

int server_send_response(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   cJSON *approved = cJSON_GetObjectItemCaseSensitive(response, "approved");
   g_sent_ok = cJSON_IsTrue(approved);
   cJSON *status = cJSON_GetObjectItemCaseSensitive(response, "status");
   g_sent_error = cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0;
   cJSON *roundtable = cJSON_GetObjectItemCaseSensitive(response, "roundtable");
   g_sent_deadline =
       cJSON_IsObject(roundtable) && cJSON_IsTrue(cJSON_GetObjectItem(roundtable, "deadline_hit"));
   cJSON *failures = cJSON_IsObject(roundtable)
                         ? cJSON_GetObjectItemCaseSensitive(roundtable, "participant_failures")
                         : NULL;
   g_sent_participant_failures = cJSON_IsArray(failures) ? cJSON_GetArraySize(failures) : 0;
   return 0;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   fprintf(stderr, "unexpected proxy error: %s\n", message ? message : "");
   return -1;
}

static void test_omitted_roundtable_forwards_saved_default(void)
{
   char home[] = "/tmp/aimee-roundtable-proxy-XXXXXX";
   assert(mkdtemp(home) != NULL);
   setenv("AIMEE_HOME", home, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);
   const char *preset_error = NULL;
   roundtable_preset_t preset;
   assert(roundtable_preset_from_json("{\"name\":\"default\",\"seats\":[{\"model\":\"codex\","
                                      "\"persona\":\"reviewer\"}],\"chairman\":\"codex\","
                                      "\"chairman_enabled\":true,\"min_successful\":1,"
                                      "\"deadline_ms\":600000}",
                                      NULL, &preset, &preset_error) == 0);
   assert(roundtable_preset_save(&preset) == 0);
   assert(config_set_roundtable_default("default") == 0);

   char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
   assert(snprintf(socket_path, sizeof(socket_path), "%s/wfe.sock", home) > 0);

   int listener = socket(AF_UNIX, SOCK_STREAM, 0);
   assert(listener >= 0);
   struct sockaddr_un addr = {.sun_family = AF_UNIX};
   assert(snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path) > 0);
   assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(listener, 1) == 0);

   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      int client = accept(listener, NULL, NULL);
      if (client < 0)
         _exit(10);
      char request[8192] = "";
      size_t used = 0;
      size_t expected = 0;
      while (used + 1 < sizeof(request))
      {
         ssize_t got = read(client, request + used, sizeof(request) - used - 1);
         if (got <= 0)
            _exit(11);
         used += (size_t)got;
         request[used] = '\0';
         expected = framed_request_size(request);
         if (expected == SIZE_MAX)
            _exit(12);
         if (expected && used >= expected)
            break;
      }
      if (!expected || used < expected ||
          !strstr(request, "POST /v1/roundtable/review HTTP/1.1\r\n") ||
          !strstr(request, "\"artifact\":\"a complete implementation artifact\"") ||
          !strstr(request, "\"roundtable\":\"default\""))
         _exit(13);

      const char *body = "{\"roundtable\":{\"approved\":true}}";
      char response[512];
      int response_n = snprintf(response, sizeof(response),
                                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                                strlen(body), body);
      if (response_n <= 0 || write(client, response, (size_t)response_n) != response_n)
         _exit(14);
      close(client);
      close(listener);
      _exit(0);
   }

   setenv("AIMEE_WFE_HTTP_SOCKET", socket_path, 1);
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "prompt", "a complete implementation artifact");
   cJSON_AddStringToObject(request, "artifact_stage", "frozen_diff");
   server_conn_t conn;
   memset(&conn, 0, sizeof(conn));
   assert(wfe_roundtable_proxy(&conn, request) == 0);
   assert(g_sent_ok == 1);
   cJSON_Delete(request);
   unsetenv("AIMEE_WFE_HTTP_SOCKET");

   int child_status = 0;
   assert(waitpid(child, &child_status, 0) == child);
   assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
   close(listener);
   assert(unlink(socket_path) == 0);
   unsetenv("AIMEE_NO_CACHE");
   unsetenv("AIMEE_HOME");
   platform_test_rmrf(home);
}

static void test_failure_retains_go_roundtable_diagnostics(void)
{
   char home[] = "/tmp/aimee-roundtable-proxy-failure-XXXXXX";
   assert(mkdtemp(home) != NULL);
   setenv("AIMEE_HOME", home, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);

   char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
   assert(snprintf(socket_path, sizeof(socket_path), "%s/wfe.sock", home) > 0);
   int listener = socket(AF_UNIX, SOCK_STREAM, 0);
   assert(listener >= 0);
   struct sockaddr_un addr = {.sun_family = AF_UNIX};
   assert(snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path) > 0);
   assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(listener, 1) == 0);

   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      int client = accept(listener, NULL, NULL);
      if (client < 0)
         _exit(20);
      char request[8192] = "";
      size_t used = 0, expected = 0;
      while (used + 1 < sizeof(request))
      {
         ssize_t got = read(client, request + used, sizeof(request) - used - 1);
         if (got <= 0)
            _exit(21);
         used += (size_t)got;
         request[used] = '\0';
         expected = framed_request_size(request);
         if (expected == SIZE_MAX)
            _exit(22);
         if (expected && used >= expected)
            break;
      }
      if (!expected || used < expected || !strstr(request, "\"roundtable\":\"deadline-e2e\""))
         _exit(23);

      const char *body = "{\"ok\":false,\"error\":\"reviewer: deadline\",\"roundtable\":{"
                         "\"deadline_hit\":true,\"participant_failures\":[{\"seat\":1,"
                         "\"category\":\"deadline\"}]}}";
      char response[1024];
      int response_n = snprintf(response, sizeof(response),
                                "HTTP/1.1 503 Service Unavailable\r\n"
                                "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                                "Connection: close\r\n\r\n%s",
                                strlen(body), body);
      if (response_n <= 0 || write(client, response, (size_t)response_n) != response_n)
         _exit(24);
      close(client);
      close(listener);
      _exit(0);
   }

   setenv("AIMEE_WFE_HTTP_SOCKET", socket_path, 1);
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "prompt", "deadline diagnostics artifact");
   cJSON_AddStringToObject(request, "roundtable", "deadline-e2e");
   server_conn_t conn;
   memset(&conn, 0, sizeof(conn));
   g_sent_ok = g_sent_error = g_sent_deadline = g_sent_participant_failures = 0;
   assert(wfe_roundtable_proxy(&conn, request) == 0);
   assert(g_sent_ok == 0);
   assert(g_sent_error == 1);
   assert(g_sent_deadline == 1);
   assert(g_sent_participant_failures == 1);
   cJSON_Delete(request);
   unsetenv("AIMEE_WFE_HTTP_SOCKET");

   int child_status = 0;
   assert(waitpid(child, &child_status, 0) == child);
   assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
   close(listener);
   assert(unlink(socket_path) == 0);
   unsetenv("AIMEE_NO_CACHE");
   unsetenv("AIMEE_HOME");
   platform_test_rmrf(home);
}

int main(void)
{
   printf("wfe_roundtable_proxy: ");
   test_omitted_roundtable_forwards_saved_default();
   test_failure_retains_go_roundtable_diagnostics();
   printf("ok\n");
   return 0;
}
