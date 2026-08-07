/* test_bus_route.c: slice 6 of the event-bus feature tree.
 *
 * Routing and the tap, checked as outcomes:
 *
 *   - The tap sees every event the host accepts, exactly once, in seq order,
 *     before routing. A client that could not enqueue (its outbound never
 *     reached the host) produced no event and is not a tap miss.
 *   - A notification reaches every authorized observer of its kind and no other
 *     client — a client never receives a kind it did not subscribe to.
 *   - A request reaches only the kind's server; its reply reaches only the
 *     original requester, matched by correlation.
 *   - A request for a kind with no server gets a synthesized capability_absent.
 *   - A cancel reaches the server.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_ring.h>
#include <aimee/core/event_bus/bus_wire.h>

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

typedef struct
{
   bus_attach_reply_t reply;
   bus_region_t control, arena, qpair;
   bus_control_t *ctl;
   bus_qpair_t qp;
} client_t;

static void attach(bus_host_t *h, uint32_t principal, client_t *c)
{
   int sv[2];
   must(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");
   bus_attach_request_t req;
   memset(&req, 0, sizeof req);
   req.magic = BUS_ATTACH_REQ_MAGIC;
   req.wire_version_min = BUS_WIRE_VERSION;
   req.wire_version_max = BUS_WIRE_VERSION;
   req.principal_ref = principal;
   must(bus_fd_send(sv[0], &req, sizeof req, NULL, 0) == 0, "send request");
   must(bus_host_serve_attach(h, sv[1]) == BUS_HOST_OK, "admitted");

   int fds[3];
   int nfd = 0;
   memset(c, 0, sizeof *c);
   must(bus_fd_recv(sv[0], &c->reply, sizeof c->reply, fds, 3, &nfd) == (long)sizeof c->reply,
        "recv reply");
   must(nfd == 3, "three fds");
   must(bus_region_map(fds[0], bus_control_bytes(), 0, &c->control) == BUS_REGION_OK, "map ctl");
   must(bus_region_map(fds[1], bus_arena_region_bytes(c->reply.arena_size), 1, &c->arena) ==
            BUS_REGION_OK,
        "map arena");
   must(bus_region_map(fds[2], bus_qpair_bytes(c->reply.slot_size, c->reply.queue_capacity), 1,
                       &c->qpair) == BUS_REGION_OK,
        "map qpair");
   must(bus_control_attach(&c->control, &c->ctl) == BUS_REGION_OK, "attach ctl");
   must(bus_qpair_attach(&c->qpair, &c->qp) == BUS_REGION_OK, "attach qp");
   for (int i = 0; i < 3; i++)
      close(fds[i]);
   close(sv[0]);
   close(sv[1]);
}

static void detach(client_t *c)
{
   bus_region_unmap(&c->control);
   bus_region_unmap(&c->arena);
   bus_region_unmap(&c->qpair);
}

/* A client emits an event into its outbound ring: header, then inline payload. */
static void emit(client_t *c, uint16_t flags, uint32_t kind, uint64_t corr, uint32_t plen,
                 uint8_t fill)
{
   uint8_t *slot = bus_ring_produce_begin(&c->qp.outbound);
   must(slot != NULL, "outbound has room");
   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.hdr_flags = flags | (plen ? BUS_F_INLINE : 0);
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = kind;
   f.correlation_id = corr;
   f.payload_len = plen;
   if (plen)
      f.payload_ref = BUS_WIRE_HDR_LEN;
   must(bus_wire_encode(&f, slot, c->reply.slot_size) == BUS_WIRE_HDR_LEN, "encode");
   if (plen)
      memset(slot + BUS_WIRE_HDR_LEN, fill, plen);
   bus_ring_produce_commit(&c->qp.outbound);
}

/* A client reads one event from its inbound ring; returns 0 if empty. */
static int recv_event(client_t *c, bus_frame_t *out, uint8_t *payload, uint32_t payload_cap)
{
   const uint8_t *slot = bus_ring_consume_begin(&c->qp.inbound);
   if (!slot)
      return 0;
   must(bus_wire_decode(slot, c->reply.slot_size, out) == BUS_WIRE_OK, "decode inbound");
   if ((out->hdr_flags & BUS_F_INLINE) && out->payload_len > 0 && payload &&
       out->payload_len <= payload_cap)
      memcpy(payload, slot + out->payload_ref, out->payload_len);
   bus_ring_consume_commit(&c->qp.inbound);
   return 1;
}

static bus_host_config_t cfg(void)
{
   bus_host_config_t c;
   memset(&c, 0, sizeof c);
   c.max_slots = 4;
   c.slot_size = 256;
   c.inline_budget = 192;
   c.queue_capacity = 16;
   c.arena_size = 128 * 1024;
   return c;
}

/* ---- tap recorder ---- */

static uint64_t g_tap_seq[256];
static uint32_t g_tap_kind[256];
static uint32_t g_tap_n;

static void recording_tap(void *ctx, const bus_frame_t *f, const uint8_t *pl, uint32_t pn)
{
   (void)pl;
   (void)pn;
   (void)ctx;
   must(g_tap_n < 256, "tap buffer");
   g_tap_seq[g_tap_n] = f->seq;
   g_tap_kind[g_tap_n] = f->event_kind;
   g_tap_n++;
}

/* ------------------------------------------------------------------ */

#define KIND_A 300
#define KIND_B 301

static void test_notification_observer_routing(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");
   g_tap_n = 0;
   bus_host_set_tap(&h, recording_tap, NULL);

   client_t pub, obs1, obs2, other;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs1);
   attach(&h, 3, &obs2);
   attach(&h, 4, &other);

   must(bus_host_subscribe(&h, obs1.reply.handle_id, KIND_A) == BUS_HOST_OK, "obs1 subscribes A");
   must(bus_host_subscribe(&h, obs2.reply.handle_id, KIND_A) == BUS_HOST_OK, "obs2 subscribes A");
   /* `other` subscribes a different kind, so it must not receive KIND_A. */
   must(bus_host_subscribe(&h, other.reply.handle_id, KIND_B) == BUS_HOST_OK, "other subs B");

   emit(&pub, BUS_F_NOTIFICATION, KIND_A, 0, 4, 0xAA);
   must(bus_host_pump(&h) == 1, "one event routed");

   bus_frame_t f;
   uint8_t buf[8];
   must(recv_event(&obs1, &f, buf, sizeof buf) == 1 && f.event_kind == KIND_A && buf[0] == 0xAA,
        "obs1 got the notification");
   must(recv_event(&obs2, &f, buf, sizeof buf) == 1 && f.event_kind == KIND_A,
        "obs2 got the notification");
   must(recv_event(&other, &f, buf, sizeof buf) == 0, "other received nothing (wrong kind)");
   must(recv_event(&pub, &f, buf, sizeof buf) == 0, "publisher received nothing");

   /* The tap saw exactly the one accepted event. */
   must(g_tap_n == 1 && g_tap_kind[0] == KIND_A, "tap saw the one event");

   detach(&pub);
   detach(&obs1);
   detach(&obs2);
   detach(&other);
   bus_host_destroy(&h);
   printf("  notifications: reach only authorized observers; tap sees the event\n");
}

static void test_request_reply(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");
   g_tap_n = 0;
   bus_host_set_tap(&h, recording_tap, NULL);

   client_t req, server, bystander;
   attach(&h, 1, &req);
   attach(&h, 2, &server);
   attach(&h, 3, &bystander);
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_A) == BUS_HOST_OK, "server serves A");
   /* A second server for the same kind is refused. */
   must(bus_host_serve_kind(&h, bystander.reply.handle_id, KIND_A) != BUS_HOST_OK,
        "second server refused");

   const uint64_t corr = 0xC0FFEE;
   emit(&req, BUS_F_REQUEST, KIND_A, corr, 4, 0x11);
   must(bus_host_pump(&h) == 1, "request routed");

   bus_frame_t f;
   uint8_t buf[8];
   must(recv_event(&server, &f, buf, sizeof buf) == 1 && (f.hdr_flags & BUS_F_REQUEST),
        "server got the request");
   /* The server is handed the host's own id, not the requester's, so that two
    * clients that pick the same number stay distinguishable. It echoes back
    * whatever it was given. */
   const uint64_t served = f.correlation_id;
   must(recv_event(&bystander, &f, buf, sizeof buf) == 0, "bystander got nothing");
   must(recv_event(&req, &f, buf, sizeof buf) == 0, "requester got nothing yet");

   /* Server replies with the correlation it was handed. */
   emit(&server, BUS_F_REPLY, KIND_A, served, 4, 0x22);
   must(bus_host_pump(&h) == 1, "reply routed");
   must(recv_event(&req, &f, buf, sizeof buf) == 1 && f.correlation_id == corr &&
            (f.hdr_flags & BUS_F_REPLY) && buf[0] == 0x22,
        "requester got the reply");
   must(recv_event(&server, &f, buf, sizeof buf) == 0, "server got nothing back");
   must(recv_event(&bystander, &f, buf, sizeof buf) == 0, "bystander still got nothing");

   detach(&req);
   detach(&server);
   detach(&bystander);
   bus_host_destroy(&h);
   printf("  request/reply: point-to-point to the server and back to the requester\n");
}

static void test_fragmented_request_reply(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t req, server;
   attach(&h, 1, &req);
   attach(&h, 2, &server);
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_A) == BUS_HOST_OK, "server serves A");
   must(bus_host_kind_has_server(&h, KIND_A), "kind reports an attached server");

   const uint64_t corr = 0xF12A6;
   bus_frame_t f;
   uint8_t buf[8];
   emit(&req, BUS_F_REQUEST | BUS_F_MORE, KIND_A, corr, 4, 0x10);
   must(bus_host_pump(&h) == 1, "first request fragment routed");
   must(recv_event(&server, &f, buf, sizeof buf) == 1 && (f.hdr_flags & BUS_F_MORE) &&
            buf[0] == 0x10,
        "server got first request fragment");
   const uint64_t served = f.correlation_id;

   /* A server cannot close the correlation before the request is complete. */
   emit(&server, BUS_F_REPLY, KIND_A, served, 4, 0x99);
   must(bus_host_pump(&h) == 1, "premature reply processed");
   must(recv_event(&req, &f, buf, sizeof buf) == 0, "premature reply dropped");

   emit(&req, BUS_F_REQUEST, KIND_A, corr, 4, 0x11);
   must(bus_host_pump(&h) == 1, "final request fragment routed");
   must(recv_event(&server, &f, buf, sizeof buf) == 1 && !(f.hdr_flags & BUS_F_MORE) &&
            buf[0] == 0x11,
        "server got final request fragment");

   emit(&server, BUS_F_REPLY | BUS_F_MORE, KIND_A, served, 4, 0x20);
   must(bus_host_pump(&h) == 1, "first reply fragment routed");
   must(recv_event(&req, &f, buf, sizeof buf) == 1 && (f.hdr_flags & BUS_F_MORE) && buf[0] == 0x20,
        "requester got first reply fragment");
   emit(&server, BUS_F_REPLY, KIND_A, served, 4, 0x21);
   must(bus_host_pump(&h) == 1, "final reply fragment routed");
   must(recv_event(&req, &f, buf, sizeof buf) == 1 && !(f.hdr_flags & BUS_F_MORE) && buf[0] == 0x21,
        "requester got final reply fragment");

   /* Cancellation retires an unfinished request, allowing its correlation to
    * be used later without leaking one pending-table entry. */
   const uint64_t cancelled = 0xCA11CE1;
   emit(&req, BUS_F_REQUEST | BUS_F_MORE, KIND_A, cancelled, 4, 0x30);
   must(bus_host_pump(&h) == 1, "partial request routed");
   must(recv_event(&server, &f, buf, sizeof buf) == 1, "server got partial request");
   emit(&req, BUS_F_CANCEL, KIND_A, cancelled, 0, 0);
   must(bus_host_pump(&h) == 1, "partial request cancelled");
   must(recv_event(&server, &f, buf, sizeof buf) == 1 && (f.hdr_flags & BUS_F_CANCEL),
        "server got partial request cancellation");
   emit(&req, BUS_F_REQUEST, KIND_A, cancelled, 4, 0x31);
   must(bus_host_pump(&h) == 1, "cancelled correlation reused");
   must(recv_event(&server, &f, buf, sizeof buf) == 1 && buf[0] == 0x31,
        "reused correlation starts a fresh request");
   emit(&server, BUS_F_REPLY, KIND_A, f.correlation_id, 4, 0x32);
   must(bus_host_pump(&h) == 1, "fresh request answered");
   must(recv_event(&req, &f, buf, sizeof buf) == 1 && buf[0] == 0x32, "fresh reply delivered");

   detach(&req);
   detach(&server);
   bus_host_destroy(&h);
   printf("  fragments: ordered request/reply lifecycle and partial cancellation\n");
}

/* A reply may come only from the kind's server, and a cancel only from the
 * original requester — a client cannot forge either for someone else's
 * correlation. */
static void test_reply_and_cancel_spoofing_refused(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t req, server, attacker;
   attach(&h, 1, &req);
   attach(&h, 2, &server);
   attach(&h, 3, &attacker);
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_A) == BUS_HOST_OK, "server serves A");

   const uint64_t corr = 0x5EED;
   emit(&req, BUS_F_REQUEST, KIND_A, corr, 0, 0);
   must(bus_host_pump(&h) == 1, "request routed");
   bus_frame_t f;
   must(recv_event(&server, &f, NULL, 0) == 1, "server got the request");
   const uint64_t served = f.correlation_id;

   /* The attacker forges a reply for the served correlation. It must be
    * dropped: it is not the server. */
   emit(&attacker, BUS_F_REPLY, KIND_A, served, 4, 0x99);
   must(bus_host_pump(&h) == 1, "forged reply processed");
   must(recv_event(&req, &f, NULL, 0) == 0, "forged reply did not reach the requester");

   /* The attacker forges a cancel for the requester's correlation. Dropped: it
    * is not the requester. */
   emit(&attacker, BUS_F_CANCEL, KIND_A, corr, 0, 0);
   must(bus_host_pump(&h) == 1, "forged cancel processed");
   must(recv_event(&server, &f, NULL, 0) == 0, "forged cancel did not reach the server");

   /* The genuine server reply still works. */
   emit(&server, BUS_F_REPLY, KIND_A, served, 4, 0x22);
   must(bus_host_pump(&h) == 1, "genuine reply routed");
   must(recv_event(&req, &f, NULL, 0) == 1 && (f.hdr_flags & BUS_F_REPLY),
        "genuine server reply reaches the requester");

   detach(&req);
   detach(&server);
   detach(&attacker);
   bus_host_destroy(&h);
   printf("  spoofing: a non-server reply and a non-requester cancel are dropped\n");
}

static void test_capability_absent(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t req;
   attach(&h, 1, &req);
   const uint64_t corr = 0xABC;
   emit(&req, BUS_F_REQUEST, KIND_A, corr, 0, 0); /* nobody serves KIND_A */
   must(bus_host_pump(&h) == 1, "request routed");

   bus_frame_t f;
   must(recv_event(&req, &f, NULL, 0) == 1 && f.event_kind == BUS_KIND_CAPABILITY_ABSENT &&
            f.correlation_id == corr && (f.hdr_flags & BUS_F_REPLY),
        "requester got a synthesized capability_absent");

   detach(&req);
   bus_host_destroy(&h);
   printf("  capability_absent: a request with no server is answered, not dropped\n");
}

static void test_cancel(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t req, server;
   attach(&h, 1, &req);
   attach(&h, 2, &server);
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_A) == BUS_HOST_OK, "server serves A");

   const uint64_t corr = 0xD00D;
   emit(&req, BUS_F_REQUEST, KIND_A, corr, 0, 0);
   must(bus_host_pump(&h) == 1, "request routed");
   bus_frame_t f;
   must(recv_event(&server, &f, NULL, 0) == 1, "server got the request");
   const uint64_t served = f.correlation_id;

   /* The requester cancels in its own numbering; the server sees the cancel
    * against the id it is actually working on. */
   emit(&req, BUS_F_CANCEL, KIND_A, corr, 0, 0);
   must(bus_host_pump(&h) == 1, "cancel routed");
   must(recv_event(&server, &f, NULL, 0) == 1 && (f.hdr_flags & BUS_F_CANCEL) &&
            f.correlation_id == served,
        "server got the cancel");

   detach(&req);
   detach(&server);
   bus_host_destroy(&h);
   printf("  cancel: delivered to the server for the outstanding correlation\n");
}

static void test_tap_order_and_completeness(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");
   g_tap_n = 0;
   bus_host_set_tap(&h, recording_tap, NULL);

   client_t pub, obs;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs);
   must(bus_host_subscribe(&h, obs.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");

   /* Emit several events, then pump once. */
   const int N = 5;
   for (int i = 0; i < N; i++)
      emit(&pub, BUS_F_NOTIFICATION, KIND_A, 0, 4, (uint8_t)i);
   must(bus_host_pump(&h) == (uint32_t)N, "all routed");

   /* The tap saw exactly N events, in strictly ascending seq order, once each. */
   must(g_tap_n == (uint32_t)N, "tap saw every accepted event exactly once");
   for (int i = 1; i < N; i++)
      must(g_tap_seq[i] == g_tap_seq[i - 1] + 1, "seq strictly ascending, no gaps");

   detach(&pub);
   detach(&obs);
   bus_host_destroy(&h);
   printf("  tap: every accepted event once, in contiguous seq order\n");
}

/* ---- arena-payload routing (D3): the host forwards a lease by reference ---- */

/* The producer's path, co-located with the host (D7): allocate a lease on the
 * host arena as this client's slot, fill it, read its generation, then emit the
 * reference frame (of the given pattern) into the outbound ring. No payload bytes
 * travel through the ring — only the lease id + generation in the header. */
static void emit_arena_pat(bus_host_t *h, client_t *pub, uint16_t pattern, uint32_t kind,
                           uint64_t corr, uint32_t len, uint8_t fill, uint32_t *lease_out,
                           uint32_t *gen_out)
{
   uint32_t lease = 0;
   must(bus_arena_alloc(&h->arena, pub->reply.handle_id, len, &lease) == BUS_ARENA_OK,
        "producer allocates a lease");
   uint8_t *p = NULL;
   must(bus_arena_fill_ptr(&h->arena, lease, &p) == BUS_ARENA_OK && p != NULL, "fill pointer");
   memset(p, fill, len);
   bus_arena_ref_t ref;
   must(bus_arena_ref(&h->arena, lease, &ref) == BUS_ARENA_OK, "reference");

   uint8_t *slot = bus_ring_produce_begin(&pub->qp.outbound);
   must(slot != NULL, "outbound has room");
   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.hdr_flags = pattern | BUS_F_ARENA;
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = kind;
   f.correlation_id = corr;
   f.payload_ref = lease; /* ARENA (v2): payload_ref is the lease id */
   f.generation = ref.generation;
   f.payload_len = len;
   must(bus_wire_encode(&f, slot, pub->reply.slot_size) == BUS_WIRE_HDR_LEN, "encode arena frame");
   bus_ring_produce_commit(&pub->qp.outbound);

   *lease_out = lease;
   *gen_out = ref.generation;
}

static void emit_arena(bus_host_t *h, client_t *pub, uint32_t kind, uint32_t len, uint8_t fill,
                       uint32_t *lease_out, uint32_t *gen_out)
{
   emit_arena_pat(h, pub, BUS_F_NOTIFICATION, kind, 0, len, fill, lease_out, gen_out);
}

/* A co-located consumer reads its arena payload in place, gated by the lease
 * table, then releases its ref. Verifies the bytes match `fill`. */
static void consume_arena(bus_host_t *h, const bus_frame_t *f, uint32_t slot, uint32_t len,
                          uint8_t fill)
{
   must((f->hdr_flags & BUS_F_ARENA) && f->payload_len == len, "frame carries the arena reference");
   const uint8_t *p = NULL;
   must(bus_arena_read_ptr(&h->arena, (uint32_t)f->payload_ref, f->generation, slot, &p) ==
                BUS_ARENA_OK &&
            p != NULL,
        "consumer reads the lease in place");
   for (uint32_t i = 0; i < len; i++)
      must(p[i] == fill, "payload bytes match");
   must(bus_arena_release(&h->arena, (uint32_t)f->payload_ref, f->generation, slot) == BUS_ARENA_OK,
        "consumer releases its ref");
}

static uint32_t data_limit(const client_t *c)
{
   uint32_t reserve = atomic_load_explicit(&c->qp.hdr->control_credits, memory_order_relaxed);
   return c->reply.queue_capacity - reserve;
}

#define ALEN 1000 /* > inline_budget (192) and > slot_size (256): genuinely arena */

/* The core: an arena notification fans out by reference to exactly the kind's
 * observers, each reads the shared bytes, and the lease drains to reclaim only
 * once every consumer has released. */
static void test_arena_notification_fanout(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");
   g_tap_n = 0;
   bus_host_set_tap(&h, recording_tap, NULL);

   client_t pub, obs1, obs2, other;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs1);
   attach(&h, 3, &obs2);
   attach(&h, 4, &other);
   must(bus_host_subscribe(&h, obs1.reply.handle_id, KIND_A) == BUS_HOST_OK, "obs1 subs A");
   must(bus_host_subscribe(&h, obs2.reply.handle_id, KIND_A) == BUS_HOST_OK, "obs2 subs A");
   must(bus_host_subscribe(&h, other.reply.handle_id, KIND_B) == BUS_HOST_OK, "other subs B");

   uint32_t lease, gen;
   emit_arena(&h, &pub, KIND_A, ALEN, 0xC7, &lease, &gen);
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 1, "producer holds the lease");

   must(bus_host_pump(&h) == 1, "one arena event routed");

   /* Published to both observers, dropping the producer ref: refcount is 2, and
    * the lease is still live (undrained) so it counts against the producer. */
   must(bus_arena_refcount(&h.arena, lease) == 2, "two consumer refs after publish");
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 1, "lease live until drained");

   bus_frame_t f;
   must(recv_event(&obs1, &f, NULL, 0) == 1 && f.event_kind == KIND_A &&
            (uint32_t)f.payload_ref == lease && f.generation == gen,
        "obs1 got the reference");
   consume_arena(&h, &f, obs1.reply.handle_id, ALEN, 0xC7);
   must(bus_arena_refcount(&h.arena, lease) == 1, "one ref left after obs1 releases");

   must(recv_event(&obs2, &f, NULL, 0) == 1 && (uint32_t)f.payload_ref == lease,
        "obs2 got the reference");
   consume_arena(&h, &f, obs2.reply.handle_id, ALEN, 0xC7);

   /* Both released: the lease is reclaimed and no longer counts against anyone. */
   must(bus_arena_refcount(&h.arena, lease) == 0, "lease drained to zero");
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 0, "producer footprint released");
   must(recv_event(&other, &f, NULL, 0) == 0, "wrong-kind client received nothing");
   must(g_tap_n == 1 && g_tap_kind[0] == KIND_A, "tap saw the one arena event");

   detach(&pub);
   detach(&obs1);
   detach(&obs2);
   detach(&other);
   bus_host_destroy(&h);
   printf("  arena: fan-out by reference; lease reclaims only after every release\n");
}

/* An arena notification with no observers must not leak: publishing to zero
 * observers reclaims the span immediately. */
static void test_arena_no_observers_reclaim(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t pub;
   attach(&h, 1, &pub);

   uint32_t lease, gen;
   emit_arena(&h, &pub, KIND_A, ALEN, 0x5A, &lease, &gen); /* nobody subscribes KIND_A */
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 1, "lease held before pump");

   must(bus_host_pump(&h) == 1, "arena event accepted");
   must(bus_arena_refcount(&h.arena, lease) == 0, "no observers: reclaimed");
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 0, "no leak");

   detach(&pub);
   bus_host_destroy(&h);
   printf("  arena: a notification with no observers reclaims its lease, no leak\n");
}

/* Under SHED, a full observer never receives the reference — so the ref
 * published to it is released, or the lease could never drain. */
static void test_arena_shed_releases_ref(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t pub, obs;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs);
   must(bus_host_subscribe(&h, obs.reply.handle_id, KIND_A) == BUS_HOST_OK, "obs subs A");
   must(bus_host_set_kind_policy(&h, KIND_A, BUS_KIND_SHED) == BUS_HOST_OK, "KIND_A sheds");

   /* Fill the observer's data ring so the arena frame cannot be delivered. */
   uint32_t lim = data_limit(&obs);
   for (uint32_t i = 0; i < lim; i++)
   {
      emit(&pub, BUS_F_NOTIFICATION, KIND_A, 0, 4, 0);
      bus_host_pump(&h);
   }

   uint32_t lease, gen;
   emit_arena(&h, &pub, KIND_A, ALEN, 0x3C, &lease, &gen);
   must(bus_host_pump(&h) == 1, "arena event accepted");

   /* The one observer was shed, its ref released: the lease is reclaimed. */
   must(bus_arena_refcount(&h.arena, lease) == 0, "shed observer's ref released");
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 0, "no leak under shed");

   /* The observer sees its data rows then an overflow — never the arena frame. */
   bus_frame_t f;
   int saw_overflow = 0, saw_arena = 0;
   while (recv_event(&obs, &f, NULL, 0) == 1)
   {
      if (f.event_kind == BUS_KIND_OVERFLOW)
         saw_overflow = 1;
      if (f.hdr_flags & BUS_F_ARENA)
         saw_arena = 1;
   }
   must(saw_overflow && !saw_arena, "observer told which seq it lost, not handed the arena frame");

   detach(&pub);
   detach(&obs);
   bus_host_destroy(&h);
   printf("  arena: a shed observer's ref is released; the lease still drains\n");
}

/* Under BLOCK, a full observer stalls the arena frame at the producer head. The
 * lease is published exactly ONCE, at first sight — retries must not re-publish
 * (that would double the refcount). When room frees, the reference is delivered. */
static void test_arena_block_publishes_once(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t pub, obs;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs);
   must(bus_host_subscribe(&h, obs.reply.handle_id, KIND_A) == BUS_HOST_OK,
        "obs subs A"); /* BLOCK */

   uint32_t lim = data_limit(&obs);
   for (uint32_t i = 0; i < lim; i++)
   {
      emit(&pub, BUS_F_NOTIFICATION, KIND_A, 0, 4, 0);
      bus_host_pump(&h);
   }

   uint32_t lease, gen;
   emit_arena(&h, &pub, KIND_A, ALEN, 0x9E, &lease, &gen);
   bus_host_pump(&h); /* obs full: publishes, then blocks */
   must(bus_arena_refcount(&h.arena, lease) == 1, "published once to the one observer");
   bus_host_pump(&h); /* still blocked: must NOT re-publish */
   must(bus_arena_refcount(&h.arena, lease) == 1, "publish is once-only across retries");

   /* Free a slot; the reference is now delivered. */
   bus_frame_t f;
   must(recv_event(&obs, &f, NULL, 0) == 1, "drain one data row to make room");
   bus_host_pump(&h);

   /* Drain to the arena frame, read and release it. */
   int delivered = 0;
   while (recv_event(&obs, &f, NULL, 0) == 1)
   {
      if (f.hdr_flags & BUS_F_ARENA)
      {
         consume_arena(&h, &f, obs.reply.handle_id, ALEN, 0x9E);
         delivered = 1;
      }
   }
   must(delivered, "the blocked arena reference was delivered after room freed");
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 0, "lease drained after release");

   detach(&pub);
   detach(&obs);
   bus_host_destroy(&h);
   printf("  arena: a blocked lease is published once and delivered when room frees\n");
}

/* Fault injection: a consumer that dies holding an arena ref must not strand the
 * lease. Reaping the dead slot drops its ref, and the survivor's release drains
 * the lease to reclaim. */
static void test_arena_reaped_consumer_drains(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t pub, obs1, obs2;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs1);
   attach(&h, 3, &obs2);
   must(bus_host_subscribe(&h, obs1.reply.handle_id, KIND_A) == BUS_HOST_OK, "obs1 subs A");
   must(bus_host_subscribe(&h, obs2.reply.handle_id, KIND_A) == BUS_HOST_OK, "obs2 subs A");

   /* Everyone beats once for a baseline (an unbeating client would itself be
    * reaped as stale). */
   atomic_store_explicit(&pub.qp.hdr->client_heartbeat, 1, memory_order_release);
   atomic_store_explicit(&obs1.qp.hdr->client_heartbeat, 1, memory_order_release);
   atomic_store_explicit(&obs2.qp.hdr->client_heartbeat, 1, memory_order_release);
   must(bus_host_reap(&h, 100, 50) == 0, "baseline: nobody stale");

   uint32_t lease, gen;
   emit_arena(&h, &pub, KIND_A, ALEN, 0x71, &lease, &gen);
   must(bus_host_pump(&h) == 1, "arena event routed");
   must(bus_arena_refcount(&h.arena, lease) == 2, "both observers hold a ref");

   /* obs1 reads and releases; obs2 dies holding its ref. */
   bus_frame_t f;
   must(recv_event(&obs1, &f, NULL, 0) == 1, "obs1 got it");
   consume_arena(&h, &f, obs1.reply.handle_id, ALEN, 0x71);
   must(bus_arena_refcount(&h.arena, lease) == 1, "obs2's ref still outstanding");

   /* obs2 goes quiet; the others keep beating. Advance past the stale window so
    * only obs2 is reaped. */
   atomic_store_explicit(&pub.qp.hdr->client_heartbeat, 2, memory_order_release);
   atomic_store_explicit(&obs1.qp.hdr->client_heartbeat, 2, memory_order_release);
   must(bus_host_reap(&h, 200, 50) == 1, "the dead consumer is reaped");
   must(bus_arena_refcount(&h.arena, lease) == 0, "reap drops the dead consumer's ref");
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 0, "lease reclaimed, not stranded");

   detach(&pub);
   detach(&obs1);
   detach(&obs2);
   bus_host_destroy(&h);
   printf("  arena: a reaped consumer's ref is dropped; the lease is not stranded\n");
}

/* The host validates an arena frame against the authoritative lease: a frame
 * whose payload_len exceeds the leased span is a producer bug (and would let a
 * consumer over-read). It is dropped-with-count, never published, so no observer
 * is handed an out-of-bounds reference. */
static void test_arena_length_mismatch_dropped(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t pub, obs;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs);
   must(bus_host_subscribe(&h, obs.reply.handle_id, KIND_A) == BUS_HOST_OK, "obs subs A");

   /* Lease ALEN bytes, but emit a frame claiming more than the span holds. */
   uint32_t lease = 0;
   must(bus_arena_alloc(&h.arena, pub.reply.handle_id, ALEN, &lease) == BUS_ARENA_OK, "alloc");
   uint8_t *p = NULL;
   must(bus_arena_fill_ptr(&h.arena, lease, &p) == BUS_ARENA_OK, "fill");
   bus_arena_ref_t ref;
   must(bus_arena_ref(&h.arena, lease, &ref) == BUS_ARENA_OK, "ref");

   uint8_t *slot = bus_ring_produce_begin(&pub.qp.outbound);
   must(slot != NULL, "outbound room");
   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.hdr_flags = BUS_F_NOTIFICATION | BUS_F_ARENA;
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = KIND_A;
   f.payload_ref = lease;
   f.generation = ref.generation;
   f.payload_len = ALEN + 100; /* lies: larger than the leased span */
   must(bus_wire_encode(&f, slot, pub.reply.slot_size) == BUS_WIRE_HDR_LEN, "encode");
   bus_ring_produce_commit(&pub.qp.outbound);

   uint64_t before = h.slots[pub.reply.handle_id].dropped;
   must(bus_host_pump(&h) == 1, "event seen once");
   must(h.slots[pub.reply.handle_id].dropped == before + 1,
        "the lying frame is dropped-with-count");

   bus_frame_t g;
   must(recv_event(&obs, &g, NULL, 0) == 0, "no observer received an out-of-bounds reference");
   /* Not published: the lease is still producer-held (it will be reaped, not leaked). */
   must(bus_arena_refcount(&h.arena, lease) == 1, "lease untouched, still producer-held");

   detach(&pub);
   detach(&obs);
   bus_host_destroy(&h);
   printf("  arena: a frame lying about its length is dropped, never routed\n");
}

/* An arena request reaches the kind's server by reference; the server's arena
 * reply reaches the original requester. Both spans drain to reclaim once read. */
static void test_arena_request_reply(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t req, server, bystander;
   attach(&h, 1, &req);
   attach(&h, 2, &server);
   attach(&h, 3, &bystander);
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_A) == BUS_HOST_OK, "server serves A");

   const uint64_t corr = 0xA5A5;
   uint32_t qlease, qgen;
   emit_arena_pat(&h, &req, BUS_F_REQUEST, KIND_A, corr, ALEN, 0x33, &qlease, &qgen);
   must(bus_host_pump(&h) == 1, "request routed");

   bus_frame_t f;
   must(recv_event(&server, &f, NULL, 0) == 1 && (f.hdr_flags & BUS_F_REQUEST) &&
            (f.hdr_flags & BUS_F_ARENA) && (uint32_t)f.payload_ref == qlease,
        "server got the arena request");
   const uint64_t served = f.correlation_id;
   must(recv_event(&bystander, &f, NULL, 0) == 0, "bystander got nothing");
   consume_arena(&h, &f, server.reply.handle_id, ALEN, 0x33);
   must(bus_arena_live_leases(&h.arena, req.reply.handle_id) == 0, "request span drained");

   /* Server replies with its own arena lease. */
   uint32_t rlease, rgen;
   emit_arena_pat(&h, &server, BUS_F_REPLY, KIND_A, served, ALEN + 200, 0x44, &rlease, &rgen);
   must(bus_host_pump(&h) == 1, "reply routed");
   must(recv_event(&req, &f, NULL, 0) == 1 && (f.hdr_flags & BUS_F_REPLY) &&
            (f.hdr_flags & BUS_F_ARENA) && f.correlation_id == corr,
        "requester got the arena reply");
   must(recv_event(&server, &f, NULL, 0) == 0, "server got nothing back");
   consume_arena(&h, &f, req.reply.handle_id, ALEN + 200, 0x44);
   must(bus_arena_live_leases(&h.arena, server.reply.handle_id) == 0, "reply span drained");

   detach(&req);
   detach(&server);
   detach(&bystander);
   bus_host_destroy(&h);
   printf("  arena: a request reaches the server and its reply the requester, by reference\n");
}

/* An arena request for a kind with no server is answered with capability_absent,
 * and its lease is reclaimed (not leaked, not routed to nobody). */
static void test_arena_request_no_server(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t req;
   attach(&h, 1, &req);
   const uint64_t corr = 0xBEEF;
   uint32_t lease, gen;
   emit_arena_pat(&h, &req, BUS_F_REQUEST, KIND_A, corr, ALEN, 0x77, &lease, &gen);
   must(bus_host_pump(&h) == 1, "request routed");

   bus_frame_t f;
   must(recv_event(&req, &f, NULL, 0) == 1 && f.event_kind == BUS_KIND_CAPABILITY_ABSENT &&
            f.correlation_id == corr,
        "requester got capability_absent");
   must(bus_arena_refcount(&h.arena, lease) == 0, "lease reclaimed, not leaked");
   must(bus_arena_live_leases(&h.arena, req.reply.handle_id) == 0, "no live lease");

   detach(&req);
   bus_host_destroy(&h);
   printf("  arena: a request with no server is answered and its lease reclaimed\n");
}

/* Only the kind's server may answer a correlation: a forged arena reply from a
 * non-server is dropped and its lease reclaimed, never delivered to the requester. */
static void test_arena_reply_forged_dropped(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t req, server, attacker;
   attach(&h, 1, &req);
   attach(&h, 2, &server);
   attach(&h, 3, &attacker);
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_A) == BUS_HOST_OK, "server serves A");

   const uint64_t corr = 0x5EED;
   uint32_t qlease, qgen;
   emit_arena_pat(&h, &req, BUS_F_REQUEST, KIND_A, corr, ALEN, 0x11, &qlease, &qgen);
   must(bus_host_pump(&h) == 1, "request routed");
   bus_frame_t f;
   must(recv_event(&server, &f, NULL, 0) == 1, "server got the request");
   const uint64_t served = f.correlation_id;
   consume_arena(&h, &f, server.reply.handle_id, ALEN, 0x11);

   /* The attacker (not the server) forges an arena reply for the correlation. */
   uint32_t flease, fgen;
   emit_arena_pat(&h, &attacker, BUS_F_REPLY, KIND_A, served, ALEN, 0x99, &flease, &fgen);
   must(bus_host_pump(&h) == 1, "forged reply processed");
   must(recv_event(&req, &f, NULL, 0) == 0, "forged reply did not reach the requester");
   must(bus_arena_refcount(&h.arena, flease) == 0, "forged reply's lease reclaimed");

   detach(&req);
   detach(&server);
   detach(&attacker);
   bus_host_destroy(&h);
   printf("  arena: a forged (non-server) reply is dropped and its lease reclaimed\n");
}

/* Correlation ids are chosen by each client independently, so two clients
 * calling the same server will pick the same number sooner or later. Each
 * requester's reply must still come back to it: before the pending table was
 * keyed by requester, the second caller's request was refused as a correlation
 * clash and its reply -- if one came -- could be delivered to the first. */
static void test_concurrent_requesters_share_a_correlation(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");
   g_tap_n = 0;

   client_t one, two, server;
   attach(&h, 1, &one);
   attach(&h, 2, &two);
   attach(&h, 3, &server);
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_A) == BUS_HOST_OK, "server serves A");

   const uint64_t corr = 1; /* what a fresh caller naturally picks first */
   emit(&one, BUS_F_REQUEST, KIND_A, corr, 4, 0x11);
   must(bus_host_pump(&h) == 1, "first request routed");
   emit(&two, BUS_F_REQUEST, KIND_A, corr, 4, 0x22);
   must(bus_host_pump(&h) == 1, "second request routed");

   bus_frame_t f;
   uint8_t buf[8];
   must(recv_event(&server, &f, buf, sizeof buf) == 1 && buf[0] == 0x11, "server got the first");
   const uint64_t served_one = f.correlation_id;
   must(recv_event(&server, &f, buf, sizeof buf) == 1 && buf[0] == 0x22,
        "server got the second, not a capability-absent refusal");
   const uint64_t served_two = f.correlation_id;
   must(served_one != served_two, "the two calls are distinguishable to the server");
   must(recv_event(&one, &f, buf, sizeof buf) == 0, "first requester not answered yet");
   must(recv_event(&two, &f, buf, sizeof buf) == 0, "second requester not answered yet");

   /* The server answers the second caller first. Each reply must reach the
    * client that asked, in that client's own numbering. */
   emit(&server, BUS_F_REPLY, KIND_A, served_two, 4, 0xBB);
   must(bus_host_pump(&h) == 1, "reply routed");
   emit(&server, BUS_F_REPLY, KIND_A, served_one, 4, 0xAA);
   must(bus_host_pump(&h) == 1, "second reply routed");

   must(recv_event(&two, &f, buf, sizeof buf) == 1 && (f.hdr_flags & BUS_F_REPLY) &&
            f.correlation_id == corr && buf[0] == 0xBB,
        "second requester got its own reply");
   must(recv_event(&one, &f, buf, sizeof buf) == 1 && (f.hdr_flags & BUS_F_REPLY) &&
            f.correlation_id == corr && buf[0] == 0xAA,
        "first requester got its own reply");

   detach(&one);
   detach(&two);
   detach(&server);
   bus_host_destroy(&h);
   printf("  concurrent requesters: a shared correlation id still routes per requester\n");
}

int main(void)
{
   printf("test_bus_route:\n");
   test_notification_observer_routing();
   test_request_reply();
   test_concurrent_requesters_share_a_correlation();
   test_fragmented_request_reply();
   test_reply_and_cancel_spoofing_refused();
   test_capability_absent();
   test_cancel();
   test_tap_order_and_completeness();
   test_arena_notification_fanout();
   test_arena_no_observers_reclaim();
   test_arena_shed_releases_ref();
   test_arena_block_publishes_once();
   test_arena_reaped_consumer_drains();
   test_arena_length_mismatch_dropped();
   test_arena_request_reply();
   test_arena_request_no_server();
   test_arena_reply_forged_dropped();
   printf("test_bus_route: OK\n");
   return 0;
}
