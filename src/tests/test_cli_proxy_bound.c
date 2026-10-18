/* A host-bound listener must never consult global CLI/provider credentials. */
#include "cli_proxy.h"
#include <aimee/core/connection/socket.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *payload = "{\"n\":9007199254740993,\"tiny\":1e-400}";
typedef struct
{
   const char *owner, *session;
   int direct_fd;
   atomic_int calls, wait_for_cancel, saw_cancel;
} host_scope_t;

static int forward(const char *method, const char *path, const char *headers, const void *body,
                   unsigned long body_len, const char *session, const cli_proxy_io_t *io,
                   void *context)
{
   host_scope_t *scope = context;
   assert(!strcmp(method, "POST") && !strcmp(path, "/v1/responses"));
   assert(!strcmp(session, scope->session));
   assert(!strstr(headers, "spoof") && !strstr(headers, "local-token"));
   assert(body_len == strlen(payload) && !memcmp(body, payload, body_len));
   atomic_fetch_add(&scope->calls, 1);
   char response[512], data[128];
   int length = snprintf(data, sizeof data, "{\"owner\":\"%s\",\"session\":\"%s\"}", scope->owner,
                         scope->session);
   int size = snprintf(response, sizeof response,
                       "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
                       "Connection: close\r\n\r\n%s",
                       length, data);
   if (scope->direct_fd)
   {
      io->response_started(io->context);
      assert(!aimee_core_socket_write_all(io->downstream_fd, response, (size_t)size));
   }
   else
      assert(!io->write_response(response, (unsigned long)size, io->context));
   if (atomic_load(&scope->wait_for_cancel))
   {
      for (int i = 0; i < 300 && !io->cancelled(io->context); i++)
         usleep(10000);
      assert(io->cancelled(io->context));
      atomic_store(&scope->saw_cancel, 1);
   }
   return 0;
}

static int send_request(unsigned port, const char *token, const char *path)
{
   char number[16];
   snprintf(number, sizeof number, "%u", port);
   int fd = aimee_core_socket_connect("127.0.0.1", number, 2000);
   assert(fd >= 0);
   assert(!aimee_core_socket_set_timeouts(fd, 4000, 2000));
   char request[1024];
   int n = snprintf(request, sizeof request,
                    "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nAuthorization: Bearer %s\r\n"
                    "session_id: spoof_session\r\nX-Aimee-Webuser: spoof\r\n"
                    "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                    path, port, token, strlen(payload), payload);
   assert(!aimee_core_socket_write_all(fd, request, (size_t)n));
   return fd;
}

static void read_response(int fd, char *response, size_t capacity)
{
   size_t used = 0;
   while (used + 1 < capacity)
   {
      long n = aimee_core_socket_read(fd, response + used, capacity - used - 1);
      assert(n >= 0);
      if (!n)
         break;
      used += (size_t)n;
   }
   response[used] = 0;
   aimee_core_socket_close(fd);
}

int main(void)
{
   unsigned a_port, b_port;
   host_scope_t a = {.owner = "user-a", .session = "session-a"};
   host_scope_t b = {.owner = "user-b", .session = "session-b", .direct_fd = 1};
   assert(!cli_proxy_start_bound(0, "local-token-a", "session-a", &a_port, NULL, &a));
   cli_proxy_t *a_listener =
       cli_proxy_start_bound(0, "local-token-a", a.session, &a_port, forward, &a);
   cli_proxy_t *b_listener =
       cli_proxy_start_bound(0, "local-token-b", b.session, &b_port, forward, &b);
   assert(a_listener && b_listener && a_port != b_port);
   char response[1024];
   for (int i = 0; i < 12; i++)
   {
      read_response(send_request(a_port, "local-token-a", "/v1/responses"), response,
                    sizeof response);
      assert(strstr(response, "200 OK") && strstr(response, "user-a") &&
             !strstr(response, "user-b"));
      assert(!strstr(response, "502") && !strstr(response, "spoof"));
      read_response(send_request(b_port, "local-token-b", "/v1/responses"), response,
                    sizeof response);
      assert(strstr(response, "200 OK") && strstr(response, "user-b") &&
             !strstr(response, "user-a"));
      assert(!strstr(response, "502") && !strstr(response, "spoof"));
   }
   int before = atomic_load(&a.calls);
   read_response(send_request(a_port, "local-token-b", "/v1/responses"), response, sizeof response);
   assert(strstr(response, "401") && atomic_load(&a.calls) == before);
   read_response(send_request(a_port, "local-token-a", "/v1/config/get"), response,
                 sizeof response);
   assert(strstr(response, "404") && atomic_load(&a.calls) == before);
   atomic_store(&a.wait_for_cancel, 1);
   int fd = send_request(a_port, "local-token-a", "/v1/responses");
   assert(aimee_core_socket_read(fd, response, sizeof response) > 0);
   aimee_core_socket_shutdown(fd);
   aimee_core_socket_close(fd);
   for (int i = 0; i < 300 && !atomic_load(&a.saw_cancel); i++)
      usleep(10000);
   assert(atomic_load(&a.saw_cancel));
   cli_proxy_stop(a_listener);
   cli_proxy_stop(b_listener);
   puts("bound model listener: owner/session isolation, framing and cancellation passed");
   return 0;
}
