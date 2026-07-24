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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bus_host.h"
#include "bus_ring.h"
#include "bus_wire.h"

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
   must(recv_event(&server, &f, buf, sizeof buf) == 1 && f.correlation_id == corr &&
            (f.hdr_flags & BUS_F_REQUEST),
        "server got the request");
   must(recv_event(&bystander, &f, buf, sizeof buf) == 0, "bystander got nothing");
   must(recv_event(&req, &f, buf, sizeof buf) == 0, "requester got nothing yet");

   /* Server replies with the same correlation. */
   emit(&server, BUS_F_REPLY, KIND_A, corr, 4, 0x22);
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

   /* The attacker forges a reply for the requester's correlation. It must be
    * dropped: it is not the server. */
   emit(&attacker, BUS_F_REPLY, KIND_A, corr, 4, 0x99);
   must(bus_host_pump(&h) == 1, "forged reply processed");
   must(recv_event(&req, &f, NULL, 0) == 0, "forged reply did not reach the requester");

   /* The attacker forges a cancel for the requester's correlation. Dropped: it
    * is not the requester. */
   emit(&attacker, BUS_F_CANCEL, KIND_A, corr, 0, 0);
   must(bus_host_pump(&h) == 1, "forged cancel processed");
   must(recv_event(&server, &f, NULL, 0) == 0, "forged cancel did not reach the server");

   /* The genuine server reply still works. */
   emit(&server, BUS_F_REPLY, KIND_A, corr, 4, 0x22);
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

   emit(&req, BUS_F_CANCEL, KIND_A, corr, 0, 0);
   must(bus_host_pump(&h) == 1, "cancel routed");
   must(recv_event(&server, &f, NULL, 0) == 1 && (f.hdr_flags & BUS_F_CANCEL) &&
            f.correlation_id == corr,
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

int main(void)
{
   printf("test_bus_route:\n");
   test_notification_observer_routing();
   test_request_reply();
   test_reply_and_cancel_spoofing_refused();
   test_capability_absent();
   test_cancel();
   test_tap_order_and_completeness();
   printf("test_bus_route: OK\n");
   return 0;
}
