/* test_cli_v1_poll_deadline.c: an async /v1 call must honour its own timeout.
 *
 * cli_v1_run_and_poll POSTs an async route, gets a run handle, then polls
 * GET /v1/runs/{id} until the run is terminal. It used to credit only its 500ms
 * sleep against the caller's budget:
 *
 *     if (timeout_ms > 0 && waited >= timeout_ms) return NULL;
 *     cli_v1_sleep_ms(step_ms);
 *     waited += step_ms;
 *
 * while each poll could block for its own 15s. A server that accepts the poll
 * and then goes quiet therefore burned 15.5s of wall clock per iteration and
 * counted 0.5s of it, so a declared 300000ms budget could run for HOURS.
 *
 * That is not a hypothetical: `aimee git` against a workspace the server could
 * not resolve sat far past the 300s printed in its own route table, which read
 * as a hang and was diagnosed as one.
 *
 * The stub below reproduces exactly that server: it answers the POST with a run
 * handle, then accepts every poll and never replies. The assertion is on ELAPSED
 * WALL CLOCK, because that is the property that was broken -- with the old
 * accounting this test runs for over a minute; with the deadline it returns
 * inside its budget.
 */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "cli_client.h"
#include "util.h" /* util_now_ms */

#define BUDGET_MS 2000
/* Generous: the point is to separate "inside the budget" from the old
 * behaviour, which needed a poll's full 15s before it even looked at the
 * clock. Anything under the first poll timeout proves the deadline is real. */
#define CEILING_MS 9000

static atomic_int g_stop;
static atomic_int g_polls;

typedef struct
{
   int listen_fd;
} stub_t;

/* Answers the first request (the POST) with a run handle, then accepts every
 * poll and holds it open without replying. Connections are kept open until the
 * test says stop, so the client sees a stalled server rather than a refusal. */
static void *stub_server(void *arg)
{
   stub_t *s = (stub_t *)arg;
   int held[64];
   int held_n = 0;
   int first = 1;

   while (!atomic_load(&g_stop))
   {
      struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
      fd_set rd;
      FD_ZERO(&rd);
      FD_SET(s->listen_fd, &rd);
      if (select(s->listen_fd + 1, &rd, NULL, NULL, &tv) <= 0)
         continue;

      int c = accept(s->listen_fd, NULL, NULL);
      if (c < 0)
         continue;

      char req[4096];
      ssize_t r = recv(c, req, sizeof(req) - 1, 0);
      if (r <= 0)
      {
         close(c);
         continue;
      }
      req[r] = '\0';

      if (first)
      {
         first = 0;
         const char *body = "{\"id\":\"oprun_1_1\"}";
         char resp[256];
         int n = snprintf(resp, sizeof(resp),
                          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                          strlen(body), body);
         ssize_t w = send(c, resp, (size_t)n, 0);
         (void)w;
         close(c);
         continue;
      }

      /* A poll. Say nothing at all and keep the socket open. */
      atomic_fetch_add(&g_polls, 1);
      if (held_n < (int)(sizeof(held) / sizeof(held[0])))
         held[held_n++] = c;
      else
         close(c);
   }

   for (int i = 0; i < held_n; i++)
      close(held[i]);
   return NULL;
}

int main(void)
{
   printf("cli_v1_poll_deadline:\n");

   stub_t stub;
   stub.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(stub.listen_fd >= 0);
   int one = 1;
   setsockopt(stub.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0; /* ephemeral: no fixed port to collide with a parallel test */
   assert(bind(stub.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(stub.listen_fd, 16) == 0);

   socklen_t alen = sizeof(addr);
   assert(getsockname(stub.listen_fd, (struct sockaddr *)&addr, &alen) == 0);
   int port = ntohs(addr.sin_port);

   pthread_t th;
   assert(pthread_create(&th, NULL, stub_server, &stub) == 0);

   char endpoint[64];
   snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d", port);
   setenv("AIMEE_API_ENDPOINT", endpoint, 1);

   /* dev.sweep is an async route, so this goes through cli_v1_run_and_poll. */
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "dev.sweep");

   long long start = util_now_ms();
   cJSON *resp = cli_v1_dispatch(req, BUDGET_MS);
   long long elapsed = util_now_ms() - start;

   cJSON_Delete(req);
   if (resp)
      cJSON_Delete(resp);

   atomic_store(&g_stop, 1);
   pthread_join(th, NULL);
   close(stub.listen_fd);
   unsetenv("AIMEE_API_ENDPOINT");

   printf("  budget=%dms elapsed=%lldms polls=%d\n", BUDGET_MS, elapsed, atomic_load(&g_polls));
   assert(atomic_load(&g_polls) >= 1); /* the run handle was accepted and polled */
   assert(elapsed < CEILING_MS);       /* the budget bounds the wall clock */

   printf("  a stalled server does not outlive the caller's timeout: ok\n");
   printf("All cli_v1_poll_deadline tests passed.\n");
   return 0;
}
