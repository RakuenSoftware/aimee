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

#define KIND_NOTIFY     1000
#define KIND_ACK        1001
#define KIND_ECHO       1002
#define KIND_NOSERVER   1003
#define KIND_CANCEL_ACK 1004

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

   /* Service external clients one at a time. When one disconnects it is reaped
    * and the next is accepted into the freed slot — which is what proves
    * reaped-client recovery across the language boundary. Bounded by the overall
    * timeout so it can never hang. */
   uint64_t start = now_ms();
   uint64_t hostclock = 0;

   while (now_ms() - start < timeout_ms)
   {
      int csock = accept(lsock, NULL, NULL);
      if (csock < 0)
         break;
      if (bus_host_serve_attach(&h, csock) != BUS_HOST_OK)
      {
         close(csock);
         continue;
      }
      int ext = find_external(&h, cc.reply.handle_id);
      if (ext < 0)
      {
         close(csock);
         continue;
      }
      /* Subscribe the (freshly-handled) external client to the notices it
       * observes: the ack for its notifications and the cancel-ack. */
      bus_host_subscribe(&h, (uint32_t)ext, KIND_ACK);
      bus_host_subscribe(&h, (uint32_t)ext, KIND_CANCEL_ACK);

      uint64_t cc_hb = 0;
      for (;;)
      {
         if (now_ms() - start >= timeout_ms)
            goto shutdown;

         /* The internal server must stay alive across the reap that collects a
          * departed external client, or KIND_ECHO would lose its server. It
          * heartbeats every iteration with an advancing value. */
         bus_client_heartbeat(&cc, ++cc_hb);
         bus_host_pump(&h);

         bus_event_t ev;
         while (bus_client_poll(&cc, &ev) == BUS_CLIENT_OK)
         {
            if (ev.frame.event_kind == KIND_NOTIFY)
               bus_client_publish(&cc, KIND_ACK, ev.payload, ev.payload_len);
            else if (ev.frame.event_kind == KIND_ECHO && (ev.frame.hdr_flags & BUS_F_REQUEST))
               bus_client_reply(&cc, KIND_ECHO, ev.frame.correlation_id, ev.payload,
                                ev.payload_len);
            else if (ev.frame.hdr_flags & BUS_F_CANCEL)
               /* The internal server received the cancel: tell the external
                * client, so cancel delivery is observable across the boundary. */
               bus_client_publish(&cc, KIND_CANCEL_ACK, NULL, 0);
         }
         bus_host_pump(&h);

         /* Peer closed? Reap it and move on to the next client. */
         char probe;
         ssize_t pr = recv(csock, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
         if (pr == 0)
         {
            close(csock);
            /* Drive the reaper: the external client never heartbeat. The first
             * tick starts its stale clock; the second, past the window, collects
             * it. The internal server heartbeats before each tick so the blanket
             * reap never mistakes it for dead. */
            bus_client_heartbeat(&cc, ++cc_hb);
            hostclock += 1000;
            bus_host_reap(&h, hostclock, 1);
            bus_client_heartbeat(&cc, ++cc_hb);
            hostclock += 1000;
            uint32_t reaped = bus_host_reap(&h, hostclock, 1);
            if (reaped == 0)
               fprintf(stderr, "warning: external client not reaped\n");
            break;
         }

         struct timespec nap = {.tv_sec = 0, .tv_nsec = 1 * 1000 * 1000};
         nanosleep(&nap, NULL);
      }
   }

shutdown:
   bus_client_detach(&cc);
   bus_host_destroy(&h);
   close(lsock);
   unlink(path);
   return 0;
}
