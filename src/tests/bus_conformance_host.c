/* bus_conformance_host.c: the C bus host, exposed on a Unix socket so an
 * external client — notably the Go reference client — can attach and interoperate
 * with the single in-source C host across a process boundary. This is the
 * cross-language conformance vehicle (slice 10): two independent client
 * implementations on one host is what keeps the wire spec honest.
 *
 * It runs the C host plus one internal C client that plays the server/subscriber
 * side, so an external client can prove both directions:
 *   - KIND_NOTIFY: the external client publishes; the internal C client receives
 *     and answers with a KIND_ACK the external client is subscribed to
 *     (external -> host -> C client -> host -> external);
 *   - KIND_ECHO: the internal C client serves; the external client's request is
 *     echoed back as a reply (external <-> C client through the host);
 *   - a request for KIND_NOSERVER draws a synthesized capability_absent.
 *
 * Usage: bus_conformance_host <socket-path> [timeout-ms]
 * It listens, accepts exactly one external client, pumps and services until the
 * external client disconnects or the timeout elapses, then exits 0. A bounded
 * timeout means it can never hang CI.
 *
 * This is a test binary, never linked into a shipping target (D7).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "bus_client.h"
#include "bus_host.h"

#define KIND_NOTIFY   1000
#define KIND_ACK      1001
#define KIND_ECHO     1002
#define KIND_NOSERVER 1003

static uint64_t now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct serve_arg
{
   bus_host_t *h;
   int fd;
};
static void *serve_thread(void *p)
{
   struct serve_arg *a = p;
   bus_host_serve_attach(a->h, a->fd);
   return NULL;
}

/* Attach the internal C client over a socketpair the host serves on a thread. */
static int attach_internal(bus_host_t *h, bus_client_t *c)
{
   int sv[2];
   if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0)
      return -1;
   struct serve_arg a = {.h = h, .fd = sv[1]};
   pthread_t t;
   if (pthread_create(&t, NULL, serve_thread, &a) != 0)
      return -1;
   bus_client_result_t rc = bus_client_attach(sv[0], c);
   pthread_join(t, NULL);
   close(sv[0]);
   close(sv[1]);
   return rc == BUS_CLIENT_OK ? 0 : -1;
}

/* Find the one admitted slot that is not the internal client's. */
static int find_external(bus_host_t *h, uint32_t internal_handle)
{
   for (uint32_t i = 0; i < h->cfg.max_slots; i++)
      if (h->slots[i].in_use && i != internal_handle)
         return (int)i;
   return -1;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "usage: %s <socket-path> [timeout-ms]\n", argv[0]);
      return 2;
   }
   const char *path = argv[1];
   uint64_t timeout_ms = (argc > 2) ? strtoull(argv[2], NULL, 10) : 10000;

   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 4;
   cfg.slot_size = 256;
   cfg.inline_budget = 192;
   cfg.queue_capacity = 16;
   cfg.arena_size = 256 * 1024;

   bus_host_t h;
   if (bus_host_create(&h, &cfg, NULL, NULL) != BUS_HOST_OK)
   {
      fprintf(stderr, "host create failed\n");
      return 1;
   }

   /* Internal C client: subscribes to KIND_NOTIFY and serves KIND_ECHO. */
   bus_client_t cc;
   if (attach_internal(&h, &cc) != 0)
   {
      fprintf(stderr, "internal client attach failed\n");
      return 1;
   }
   bus_host_subscribe(&h, cc.reply.handle_id, KIND_NOTIFY);
   bus_host_serve_kind(&h, cc.reply.handle_id, KIND_ECHO);

   /* Listening socket. */
   unlink(path);
   int lsock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
   if (lsock < 0)
   {
      perror("socket");
      return 1;
   }
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof addr);
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
   if (bind(lsock, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(lsock, 1) != 0)
   {
      perror("bind/listen");
      return 1;
   }

   /* Signal readiness: the file existing is the readiness marker the runner
    * waits on. Accept one external client (blocking, but bounded by the runner's
    * own timeout on the whole process). */
   int csock = accept(lsock, NULL, NULL);
   if (csock < 0)
   {
      perror("accept");
      return 1;
   }
   if (bus_host_serve_attach(&h, csock) != BUS_HOST_OK)
   {
      fprintf(stderr, "external attach denied\n");
      return 1;
   }
   int ext = find_external(&h, cc.reply.handle_id);
   if (ext < 0)
   {
      fprintf(stderr, "external slot not found\n");
      return 1;
   }
   /* The external client is subscribed to the ack kind so it can observe that
    * the C client received its notification. */
   bus_host_subscribe(&h, (uint32_t)ext, KIND_ACK);

   /* Pump + service loop, bounded. */
   uint64_t start = now_ms();
   while (now_ms() - start < timeout_ms)
   {
      bus_host_pump(&h);
      cc.reply.host_epoch = bus_control_epoch(h.control); /* keep epoch fresh */

      bus_event_t ev;
      while (bus_client_poll(&cc, &ev) == BUS_CLIENT_OK)
      {
         if (ev.frame.event_kind == KIND_NOTIFY)
         {
            /* Prove receipt: answer with an ack the external client sees. */
            bus_client_publish(&cc, KIND_ACK, ev.payload, ev.payload_len);
         }
         else if (ev.frame.event_kind == KIND_ECHO && (ev.frame.hdr_flags & BUS_F_REQUEST))
         {
            bus_client_reply(&cc, KIND_ECHO, ev.frame.correlation_id, ev.payload,
                             ev.payload_len);
         }
      }
      bus_host_pump(&h);

      /* If the external client has gone, stop. A zero-length recv (peer closed)
       * is detected by a non-blocking peek. */
      char probe;
      ssize_t pr = recv(csock, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
      if (pr == 0)
         break; /* peer closed */

      struct timespec nap = {.tv_sec = 0, .tv_nsec = 1 * 1000 * 1000};
      nanosleep(&nap, NULL);
   }

   bus_client_detach(&cc);
   bus_host_destroy(&h);
   close(csock);
   close(lsock);
   unlink(path);
   return 0;
}
