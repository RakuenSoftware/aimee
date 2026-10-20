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
 *   - a request for KIND_NOSERVER draws a synthesized capability_absent;
 *   - KIND_ARENA_ASK: the internal C client, a co-located producer, leases an
 *     arena span of the asked size, fills it with a known pattern, and publishes
 *     it as a KIND_ARENA_DATA arena notification the external client observes.
 *     The external client reads it across the process boundary through the
 *     lease view and returns the ref (D12); KIND_ARENA_STATS reports how many
 *     leases the producer still has live, so the release is observable;
 *   - KIND_ARENA_SERVE_ASK: the C side sends the external client an arena-placed
 *     REQUEST of KIND_ARENA_SERVE, which the external client serves; its inline
 *     verdict on the bytes is forwarded as the ask's reply;
 *   - KIND_ARENA_REPLY_ASK: the C side answers with an arena-placed REPLY. The
 *     host routes all three arena patterns, so all three are proven here.
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

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_ring.h>

#define KIND_NOTIFY          1000
#define KIND_ACK             1001
#define KIND_ECHO            1002
#define KIND_NOSERVER        1003
#define KIND_CANCEL_ACK      1004
#define KIND_ARENA_ASK       1005
#define KIND_ARENA_DATA      1006
#define KIND_ARENA_STATS     1007
#define KIND_ARENA_SERVE_ASK 1008 /* external asks: the C side sends it an arena REQUEST */
#define KIND_ARENA_SERVE     1009 /* served by the external client */
#define KIND_ARENA_REPLY_ASK 1010 /* external asks: the C side answers with an arena REPLY */

/* Lease and fill `len` arena bytes with the known pattern, as the internal
 * co-located producer. Returns 0 on success. */
static int lease_filled(bus_host_t *h, bus_client_t *cc, uint32_t len, uint32_t *lease,
                        uint32_t *generation)
{
   uint8_t *fill = NULL;
   bus_arena_ref_t ref;
   if (len == 0 || bus_arena_alloc(&h->arena, cc->reply.handle_id, len, lease) != BUS_ARENA_OK ||
       bus_arena_fill_ptr(&h->arena, *lease, &fill) != BUS_ARENA_OK ||
       bus_arena_ref(&h->arena, *lease, &ref) != BUS_ARENA_OK)
      return -1;
   for (uint32_t i = 0; i < len; i++)
      fill[i] = (uint8_t)(i * 7u + 3u);
   *generation = ref.generation;
   return 0;
}

/* Emit an arena-placed request or reply. The client library publishes arena
 * notifications only (no shipping producer sends the others), so the internal
 * producer writes the frame itself; the host routes it like any other. */
static int send_arena_frame(bus_client_t *cc, uint16_t pattern, uint32_t kind, uint64_t corr,
                            uint32_t lease, uint32_t generation, uint32_t len)
{
   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.hdr_flags = pattern | BUS_F_ARENA;
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = kind;
   f.correlation_id = corr;
   f.src_handle = cc->reply.handle_id;
   f.payload_ref = lease;
   f.payload_len = len;
   f.generation = generation;
   uint8_t *slot = bus_ring_produce_begin(&cc->qp.outbound);
   if (!slot || bus_wire_encode(&f, slot, cc->reply.slot_size) != BUS_WIRE_HDR_LEN)
      return -1;
   bus_ring_produce_commit(&cc->qp.outbound);
   return 0;
}

static uint32_t le32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

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
   bus_host_serve_kind(&h, cc.reply.handle_id, KIND_ARENA_ASK);
   bus_host_serve_kind(&h, cc.reply.handle_id, KIND_ARENA_STATS);
   bus_host_serve_kind(&h, cc.reply.handle_id, KIND_ARENA_SERVE_ASK);
   bus_host_serve_kind(&h, cc.reply.handle_id, KIND_ARENA_REPLY_ASK);
   uint64_t serve_ask_corr = 0;               /* the external ask awaiting the serve verdict */
   uint64_t serve_req_corr = 0x5E00000000ULL; /* the C side's own request correlations */

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
      bus_host_subscribe(&h, (uint32_t)ext, KIND_ARENA_DATA);
      bus_host_serve_kind(&h, (uint32_t)ext, KIND_ARENA_SERVE);

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
            else if (ev.frame.event_kind == KIND_ARENA_ASK &&
                     (ev.frame.hdr_flags & BUS_F_REQUEST) && ev.payload_len == 4)
            {
               uint32_t len = le32(ev.payload);
               uint32_t lease = 0;
               uint8_t *fill = NULL;
               bus_arena_ref_t ref;
               uint8_t status = 1;
               if (len > 0 &&
                   bus_arena_alloc(&h.arena, cc.reply.handle_id, len, &lease) == BUS_ARENA_OK &&
                   bus_arena_fill_ptr(&h.arena, lease, &fill) == BUS_ARENA_OK &&
                   bus_arena_ref(&h.arena, lease, &ref) == BUS_ARENA_OK)
               {
                  for (uint32_t i = 0; i < len; i++)
                     fill[i] = (uint8_t)(i * 7u + 3u);
                  status = bus_client_publish_arena(&cc, KIND_ARENA_DATA, lease, ref.generation,
                                                    len) == BUS_CLIENT_OK
                               ? 0
                               : 1;
               }
               bus_client_reply(&cc, KIND_ARENA_ASK, ev.frame.correlation_id, &status, 1);
            }
            else if (ev.frame.event_kind == KIND_ARENA_SERVE_ASK &&
                     (ev.frame.hdr_flags & BUS_F_REQUEST) && ev.payload_len == 4)
            {
               /* Send the external client an arena-placed REQUEST; its verdict
                * on the bytes comes back as the reply, forwarded below. */
               uint32_t lease = 0, gen = 0, len = le32(ev.payload);
               uint8_t refused = 0xFF;
               if (lease_filled(&h, &cc, len, &lease, &gen) == 0 &&
                   send_arena_frame(&cc, BUS_F_REQUEST, KIND_ARENA_SERVE, ++serve_req_corr, lease,
                                    gen, len) == 0)
                  serve_ask_corr = ev.frame.correlation_id;
               else
                  bus_client_reply(&cc, KIND_ARENA_SERVE_ASK, ev.frame.correlation_id, &refused, 1);
            }
            else if (ev.frame.event_kind == KIND_ARENA_SERVE &&
                     (ev.frame.hdr_flags & BUS_F_REPLY) &&
                     ev.frame.correlation_id == serve_req_corr && serve_ask_corr)
            {
               bus_client_reply(&cc, KIND_ARENA_SERVE_ASK, serve_ask_corr, ev.payload,
                                ev.payload_len);
               serve_ask_corr = 0;
            }
            else if (ev.frame.event_kind == KIND_ARENA_REPLY_ASK &&
                     (ev.frame.hdr_flags & BUS_F_REQUEST) && ev.payload_len == 4)
            {
               /* Answer with an arena-placed REPLY. */
               uint32_t lease = 0, gen = 0, len = le32(ev.payload);
               uint8_t refused = 0xFF;
               if (lease_filled(&h, &cc, len, &lease, &gen) != 0 ||
                   send_arena_frame(&cc, BUS_F_REPLY, KIND_ARENA_REPLY_ASK, ev.frame.correlation_id,
                                    lease, gen, len) != 0)
                  bus_client_reply(&cc, KIND_ARENA_REPLY_ASK, ev.frame.correlation_id, &refused, 1);
            }
            else if (ev.frame.event_kind == KIND_ARENA_STATS &&
                     (ev.frame.hdr_flags & BUS_F_REQUEST))
            {
               uint32_t live = bus_arena_live_leases(&h.arena, cc.reply.handle_id);
               uint8_t out[4] = {(uint8_t)live, (uint8_t)(live >> 8), (uint8_t)(live >> 16),
                                 (uint8_t)(live >> 24)};
               bus_client_reply(&cc, KIND_ARENA_STATS, ev.frame.correlation_id, out, sizeof out);
            }
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
