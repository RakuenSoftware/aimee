/* test_cli_v1_uds_timeout.c: the co-located UDS transport must give up.
 *
 * cli_v1_send takes a timeout_ms and honoured it only on its REMOTE branch. The
 * local Unix-socket branch (cli_v1_http_request) took no timeout argument at
 * all, and its read loop was a bare blocking read():
 *
 *     int n = (int)read(fd, resp + len, 4096);
 *     if (n <= 0) break;
 *
 * so a co-located server that accepted the connection and then went quiet --
 * or died between accept and reply -- hung the CLI forever. There was no
 * budget to exceed: the caller passed one and this path dropped it. That is
 * the DEFAULT transport for a co-located install, which is what makes it worth
 * a test rather than a note.
 *
 * The stub below is that server: it binds <AIMEE_HOME>/aimee-http.sock, accepts,
 * and never writes a byte. The assertion is on elapsed wall clock -- before the
 * fix this test does not fail, it HANGS, which is precisely the symptom.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "cJSON.h"
#include "cli_client.h"
#include "util.h" /* util_now_ms */

#define BUDGET_MS  1500
#define CEILING_MS 8000

static atomic_int g_stop;
static atomic_int g_accepted;
static char g_sock[256];

static void *silent_server(void *unused)
{
   (void)unused;
   int srv = socket(AF_UNIX, SOCK_STREAM, 0);
   if (srv < 0)
      return NULL;
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_sock);
   unlink(g_sock);
   if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 8) != 0)
   {
      close(srv);
      return NULL;
   }

   int held[32];
   int held_n = 0;
   while (!atomic_load(&g_stop))
   {
      struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
      fd_set rd;
      FD_ZERO(&rd);
      FD_SET(srv, &rd);
      if (select(srv + 1, &rd, NULL, NULL, &tv) <= 0)
         continue;
      int c = accept(srv, NULL, NULL);
      if (c < 0)
         continue;
      atomic_fetch_add(&g_accepted, 1);
      /* Accepted and deliberately mute: hold it open, answer nothing. */
      if (held_n < (int)(sizeof(held) / sizeof(held[0])))
         held[held_n++] = c;
      else
         close(c);
   }
   for (int i = 0; i < held_n; i++)
      close(held[i]);
   close(srv);
   unlink(g_sock);
   return NULL;
}

int main(void)
{
   printf("cli_v1_uds_timeout:\n");

   /* AIMEE_HOME decides where the client looks for aimee-http.sock. */
   const char *tmp = getenv("TMPDIR");
   char home[192];
   snprintf(home, sizeof(home), "%s/aimee-uds-timeout-%d", tmp && tmp[0] ? tmp : "/tmp",
            (int)getpid());
   assert(mkdir(home, 0700) == 0 || 1);
   setenv("AIMEE_HOME", home, 1);
   snprintf(g_sock, sizeof(g_sock), "%s/aimee-http.sock", home);

   pthread_t th;
   assert(pthread_create(&th, NULL, silent_server, NULL) == 0);
   for (int i = 0; i < 200 && access(g_sock, F_OK) != 0; i++)
      usleep(10000); /* wait for bind/listen */

   /* A synchronous method, so this takes the plain request path rather than
    * the async poller: no remote endpoint is configured, so it goes over the
    * co-located Unix socket. */
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "server.info");

   long long start = util_now_ms();
   cJSON *resp = cli_v1_dispatch(req, BUDGET_MS);
   long long elapsed = util_now_ms() - start;

   cJSON_Delete(req);
   if (resp)
      cJSON_Delete(resp);

   atomic_store(&g_stop, 1);
   pthread_join(th, NULL);
   rmdir(home);

   printf("  budget=%dms elapsed=%lldms accepted=%d\n", BUDGET_MS, elapsed,
          atomic_load(&g_accepted));
   assert(atomic_load(&g_accepted) >= 1); /* the stub really was talked to */
   assert(elapsed < CEILING_MS);          /* and the client gave up on its own */

   printf("  a mute co-located server does not hang the client: ok\n");
   printf("All cli_v1_uds_timeout tests passed.\n");
   return 0;
}
