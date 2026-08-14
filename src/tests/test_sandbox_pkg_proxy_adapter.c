/* Exercise the retained C transport adapter with controlled Go-policy replies.
 * The Go policy tests own request/SSRF semantics; these tests pin the language
 * boundary: request fields, strict reply validation, fail-closed behavior, and
 * consumption of the sanitized forward_head returned by the module. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <aimee/core/event_bus/module_client.h>
#include <aimee/sandbox/module_api.h>

#include "cJSON.h"
#include "log.h"
#include "module_json_call.h"
#include "sandbox_pkg_proxy.h"

typedef enum
{
   REPLY_UNAVAILABLE,
   REPLY_MALFORMED,
   REPLY_DENIED,
   REPLY_INVALID,
   REPLY_BLOCKED_ADDRESS,
   REPLY_MALFORMED_ADDRESS,
   REPLY_ALLOWED,
} reply_mode_t;

static reply_mode_t g_mode;
static int g_upstream_port;
static int g_request_calls;
static int g_address_calls;
static char g_request_line[512];
static char g_allowlist[128];
static char g_checked_ip[INET6_ADDRSTRLEN];

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}

static cJSON *request_reply(int allowed)
{
   cJSON *reply = cJSON_CreateObject();
   assert(reply != NULL);
   cJSON_AddNumberToObject(reply, "kind", 3); /* absolute-form HTTP */
   cJSON_AddStringToObject(reply, "host", "127.0.0.1");
   cJSON_AddNumberToObject(reply, "port", g_upstream_port > 0 ? g_upstream_port : 80);
   cJSON_AddBoolToObject(reply, "allowed", allowed);
   if (allowed)
      cJSON_AddStringToObject(reply, "forward_head",
                              "GET /clean HTTP/1.1\r\nHost: registry.example\r\n\r\n");
   return reply;
}

cJSON *aimee_module_json_call(uint32_t event_kind, uint32_t stage_id, cJSON *request,
                              size_t max_body, int timeout_ms, aimee_module_call_result_t *result)
{
   assert(request != NULL);
   assert(max_body == 256u * 1024u);
   assert(timeout_ms == 5000);
   assert(result == NULL);

   if (event_kind == AIMEE_SANDBOX_EVENT_PROXY_REQUEST)
   {
      assert(stage_id == AIMEE_SANDBOX_STAGE_PROXY_REQUEST);
      g_request_calls++;
      const char *line = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "line"));
      const char *allowlist =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "allowlist"));
      assert(line != NULL);
      snprintf(g_request_line, sizeof g_request_line, "%s", line);
      snprintf(g_allowlist, sizeof g_allowlist, "%s", allowlist ? allowlist : "");
      cJSON_Delete(request);
      if (g_mode == REPLY_UNAVAILABLE)
         return NULL;
      if (g_mode == REPLY_MALFORMED)
         return cJSON_Parse("{\"kind\":\"absolute\",\"allowed\":true}");
      if (g_mode == REPLY_INVALID)
         return cJSON_Parse("{\"kind\":0,\"host\":\"\",\"port\":0,\"allowed\":false}");
      return request_reply(g_mode != REPLY_DENIED);
   }

   assert(event_kind == AIMEE_SANDBOX_EVENT_PROXY_ADDRESS);
   assert(stage_id == AIMEE_SANDBOX_STAGE_PROXY_ADDRESS);
   g_address_calls++;
   const char *ip = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "ip"));
   assert(ip != NULL);
   snprintf(g_checked_ip, sizeof g_checked_ip, "%s", ip);
   cJSON_Delete(request);
   if (g_mode == REPLY_MALFORMED_ADDRESS)
      return cJSON_Parse("{}");
   cJSON *reply = cJSON_CreateObject();
   assert(reply != NULL);
   cJSON_AddBoolToObject(reply, "blocked", g_mode == REPLY_BLOCKED_ADDRESS);
   return reply;
}

static void reset(reply_mode_t mode)
{
   g_mode = mode;
   g_upstream_port = 0;
   g_request_calls = 0;
   g_address_calls = 0;
   g_request_line[0] = '\0';
   g_allowlist[0] = '\0';
   g_checked_ip[0] = '\0';
}

static void expect_status(reply_mode_t mode, const char *status)
{
   static const char head[] = "GET http://127.0.0.1/pkg HTTP/1.1\r\nHost: attacker.invalid\r\n\r\n";
   int pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   reset(mode);
   assert(sandbox_pkg_proxy_serve(pair[0], 1, head, NULL, "test") == 0);
   char response[128] = "";
   ssize_t n = recv(pair[1], response, sizeof response - 1, MSG_DONTWAIT);
   assert(n > 0);
   response[n] = '\0';
   assert(strstr(response, status) != NULL);
   assert(g_request_calls == 1);
   assert(strcmp(g_request_line, head) == 0);
   assert(g_allowlist[0] == '\0');
   close(pair[0]);
   close(pair[1]);
}

static int upstream_listener(void)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_in address;
   memset(&address, 0, sizeof address);
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   address.sin_port = 0;
   assert(bind(fd, (const struct sockaddr *)&address, sizeof address) == 0);
   assert(listen(fd, 1) == 0);
   socklen_t length = sizeof address;
   assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
   g_upstream_port = ntohs(address.sin_port);
   return fd;
}

static void test_allowed_forward(void)
{
   static const char client_head[] =
       "GET http://127.0.0.1/dirty HTTP/1.1\r\nAuthorization: secret\r\n\r\n";
   static const char clean_head[] = "GET /clean HTTP/1.1\r\nHost: registry.example\r\n\r\n";
   static const char upstream_response[] = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";

   reset(REPLY_ALLOWED);
   int listener = upstream_listener();
   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      int connected = accept(listener, NULL, NULL);
      assert(connected >= 0);
      char received[256] = "";
      ssize_t n = read(connected, received, sizeof received - 1);
      assert(n == (ssize_t)strlen(clean_head));
      received[n] = '\0';
      assert(strcmp(received, clean_head) == 0);
      assert(write(connected, upstream_response, sizeof upstream_response - 1) ==
             (ssize_t)(sizeof upstream_response - 1));
      close(connected);
      close(listener);
      _exit(0);
   }

   int pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   assert(sandbox_pkg_proxy_serve(pair[0], 1, client_head, "registry.example", "test") == 0);
   char response[128] = "";
   ssize_t n = read(pair[1], response, sizeof response - 1);
   assert(n > 0);
   response[n] = '\0';
   assert(strstr(response, "204 No Content") != NULL);
   assert(g_request_calls == 1);
   assert(g_address_calls == 1);
   assert(strcmp(g_allowlist, "registry.example") == 0);
   assert(strcmp(g_checked_ip, "127.0.0.1") == 0);
   int status = 0;
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   close(pair[0]);
   close(pair[1]);
   close(listener);
}

int main(void)
{
   /* The public-listener guard must return before consulting policy. */
   int pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   reset(REPLY_ALLOWED);
   assert(sandbox_pkg_proxy_serve(pair[0], 0, "GET / HTTP/1.1\r\n\r\n", NULL, NULL) == 0);
   char unused;
   assert(recv(pair[1], &unused, 1, MSG_DONTWAIT) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
   assert(g_request_calls == 0);
   close(pair[0]);
   close(pair[1]);

   expect_status(REPLY_UNAVAILABLE, "502 Bad Gateway");
   expect_status(REPLY_MALFORMED, "502 Bad Gateway");
   expect_status(REPLY_DENIED, "502 Bad Gateway");
   expect_status(REPLY_INVALID, "400 Bad Request");

   expect_status(REPLY_BLOCKED_ADDRESS, "502 Bad Gateway");
   assert(g_address_calls == 1);
   assert(strcmp(g_checked_ip, "127.0.0.1") == 0);
   expect_status(REPLY_MALFORMED_ADDRESS, "502 Bad Gateway");
   assert(g_address_calls == 1);

   test_allowed_forward();
   puts("test_sandbox_pkg_proxy_adapter: OK");
   return 0;
}
