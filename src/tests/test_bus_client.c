/* test_bus_client.c: slice 8 of the event-bus feature tree.
 *
 * The C reference client, driven against a real host in one process. Once
 * attached, host and client share memory, so the test pumps the host's routing
 * by hand between client calls. It exercises every message pattern and the
 * backpressure path through the library rather than the raw rings.
 *
 * The attach handshake is the one place send and recv must cross, so it runs on
 * a helper thread: bus_client_attach (send-then-recv) on the main thread and
 * bus_host_serve_attach (recv-then-send) on the helper, both over a
 * SOCK_SEQPACKET socketpair. After that everything is single-threaded.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_ring.h>

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

static void must_rc(bus_client_result_t got, bus_client_result_t want, const char *what)
{
   if (got != want)
   {
      fprintf(stderr, "FAIL: %s: expected %s got %s\n", what, bus_client_result_name(want),
              bus_client_result_name(got));
      abort();
   }
}

struct serve_arg
{
   bus_host_t *h;
   int fd;
   bus_host_result_t result;
};

static void *serve_thread(void *p)
{
   struct serve_arg *a = p;
   a->result = bus_host_serve_attach(a->h, a->fd);
   return NULL;
}

/* Attach one client, returning the client result. The host end of the socket is
 * served on a helper thread so the single-process handshake does not deadlock. */
static bus_client_result_t attach_client(bus_host_t *h, bus_client_t *c)
{
   int sv[2];
   must(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");
   struct serve_arg a = {.h = h, .fd = sv[1], .result = BUS_HOST_ERR_ARG};
   pthread_t t;
   must(pthread_create(&t, NULL, serve_thread, &a) == 0, "spawn serve");
   bus_client_result_t rc = bus_client_attach(sv[0], c);
   must(pthread_join(t, NULL) == 0, "join serve");
   close(sv[0]);
   close(sv[1]);
   return rc;
}

static bus_host_config_t cfg(void)
{
   bus_host_config_t c;
   memset(&c, 0, sizeof c);
   c.max_slots = 4;
   c.slot_size = 256;
   c.inline_budget = 192;
   c.queue_capacity = 8;
   c.arena_size = 128 * 1024;
   return c;
}

#define KIND_A 500

static void test_publish_and_poll(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");

   bus_client_t pub, sub;
   must_rc(attach_client(&h, &pub), BUS_CLIENT_OK, "publisher attaches");
   must_rc(attach_client(&h, &sub), BUS_CLIENT_OK, "subscriber attaches");
   must(bus_host_subscribe(&h, sub.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");

   const char *msg = "hello-bus";
   must_rc(bus_client_publish(&pub, KIND_A, msg, (uint32_t)strlen(msg)), BUS_CLIENT_OK, "publish");

   /* Nothing arrives until the host routes. */
   bus_event_t ev;
   must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_EMPTY, "nothing before pump");
   must(bus_host_pump(&h) == 1, "host routes the notification");

   must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_OK, "subscriber polls the event");
   must(ev.frame.event_kind == KIND_A, "right kind");
   must(ev.payload_len == strlen(msg) && memcmp(ev.payload, msg, ev.payload_len) == 0,
        "zero-copy payload matches");
   must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_EMPTY, "only one event");

   bus_client_detach(&pub);
   bus_client_detach(&sub);
   bus_host_destroy(&h);
   printf("  publish/poll: a notification reaches the subscriber zero-copy\n");
}

/* The arena producer/consumer path through the client library: the producer
 * allocates a lease on the (co-located) host arena, fills it, and emits the
 * reference frame with bus_client_publish_arena — no bytes cross the ring. The
 * host routes it by reference. The subscriber is an ordinary client, as it would
 * be in another process: poll resolves the payload through the read-only lease
 * view (D12) and points at it in place, and the next poll returns the ref with a
 * lease-release event the host consumes. */
static void test_publish_arena(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");

   bus_client_t pub, sub;
   must_rc(attach_client(&h, &pub), BUS_CLIENT_OK, "publisher attaches");
   must_rc(attach_client(&h, &sub), BUS_CLIENT_OK, "subscriber attaches");
   must(bus_client_arena_readable(&sub), "a D12 host advertises the lease view");
   must(bus_host_subscribe(&h, sub.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");

   /* A zero-length arena payload is refused: there is nothing to lease. */
   must_rc(bus_client_publish_arena(&pub, KIND_A, 0, 0, 0), BUS_CLIENT_ERR_PAYLOAD,
           "empty refused");

   const uint32_t len = cf.inline_budget + 500; /* too big for inline: genuinely arena */
   uint32_t lease = 0;
   must(bus_arena_alloc(&h.arena, pub.reply.handle_id, len, &lease) == BUS_ARENA_OK, "alloc lease");
   uint8_t *p = NULL;
   must(bus_arena_fill_ptr(&h.arena, lease, &p) == BUS_ARENA_OK, "fill ptr");
   for (uint32_t i = 0; i < len; i++)
      p[i] = (uint8_t)(i * 7u + 3u);
   bus_arena_ref_t ref;
   must(bus_arena_ref(&h.arena, lease, &ref) == BUS_ARENA_OK, "ref");

   must_rc(bus_client_publish_arena(&pub, KIND_A, lease, ref.generation, len), BUS_CLIENT_OK,
           "publish arena");

   bus_event_t ev;
   must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_EMPTY, "nothing before pump");
   must(bus_host_pump(&h) == 1, "host routes the arena notification");

   must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_OK, "subscriber polls the reference");
   must(ev.frame.event_kind == KIND_A && (ev.frame.hdr_flags & BUS_F_ARENA), "arena frame");
   must((uint32_t)ev.frame.payload_ref == lease && ev.frame.generation == ref.generation &&
            ev.frame.payload_len == len,
        "the lease reference travelled intact");
   must(ev.payload != NULL && ev.payload_len == len, "poll resolves the arena payload (D12)");
   int intact = 1;
   for (uint32_t i = 0; i < len; i++)
      intact &= ev.payload[i] == (uint8_t)(i * 7u + 3u);
   must(intact, "the subscriber reads the producer's bytes in place");
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 1, "still held while being read");

   /* The next poll returns the ref; the host consumes the release. */
   must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_EMPTY, "only one event");
   must(sub.release_count == 0, "the release left the client");
   bus_host_pump(&h);
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 0, "lease drained after release");

   bus_client_detach(&pub);
   bus_client_detach(&sub);
   bus_host_destroy(&h);
   printf("  publish_arena: a large payload routes by lease, is read in place, and is released\n");
}

/* A release can only return the sender's own ref. Releasing a lease the client
 * does not hold, or with a stale generation, is dropped and the lease lives on;
 * a malformed release is dropped too. None of it is routed. */
static void test_release_is_holder_checked(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t pub, sub, other;
   must_rc(attach_client(&h, &pub), BUS_CLIENT_OK, "publisher attaches");
   must_rc(attach_client(&h, &sub), BUS_CLIENT_OK, "subscriber attaches");
   must_rc(attach_client(&h, &other), BUS_CLIENT_OK, "bystander attaches");
   must(bus_host_subscribe(&h, sub.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");
   must(bus_host_subscribe(&h, other.reply.handle_id, BUS_KIND_LEASE_RELEASE) == BUS_HOST_OK,
        "a would-be observer of releases");

   uint32_t lease = 0;
   must(bus_arena_alloc(&h.arena, pub.reply.handle_id, 600, &lease) == BUS_ARENA_OK, "alloc");
   bus_arena_ref_t ref;
   must(bus_arena_ref(&h.arena, lease, &ref) == BUS_ARENA_OK, "ref");
   must_rc(bus_client_publish_arena(&pub, KIND_A, lease, ref.generation, 600), BUS_CLIENT_OK,
           "publish");
   bus_host_pump(&h);

   uint8_t forged[BUS_LEASE_RELEASE_LEN];
   for (unsigned i = 0; i < 4; i++)
   {
      forged[i] = (uint8_t)(lease >> (8u * i));
      forged[4 + i] = (uint8_t)(ref.generation >> (8u * i));
   }
   /* The bystander never received the frame, so it holds no ref to return. */
   must_rc(bus_client_publish(&other, BUS_KIND_LEASE_RELEASE, forged, sizeof forged), BUS_CLIENT_OK,
           "bystander sends a release");
   /* Stale generation, from the real holder. */
   forged[4] ^= 0xff;
   must_rc(bus_client_publish(&sub, BUS_KIND_LEASE_RELEASE, forged, sizeof forged), BUS_CLIENT_OK,
           "holder sends a stale release");
   /* Malformed: wrong length. */
   must_rc(bus_client_publish(&sub, BUS_KIND_LEASE_RELEASE, forged, 4), BUS_CLIENT_OK,
           "holder sends a short release");
   /* A lease id past the table. */
   uint8_t past[BUS_LEASE_RELEASE_LEN] = {0x88, 0x13, 0, 0, 1, 0, 0, 0}; /* 5000, gen 1 */
   must_rc(bus_client_publish(&sub, BUS_KIND_LEASE_RELEASE, past, sizeof past), BUS_CLIENT_OK,
           "holder sends a release for lease 5000");
   uint64_t dropped_before =
       h.slots[other.reply.handle_id].dropped + h.slots[sub.reply.handle_id].dropped;
   bus_host_pump(&h);
   uint64_t dropped_after =
       h.slots[other.reply.handle_id].dropped + h.slots[sub.reply.handle_id].dropped;
   must(dropped_after == dropped_before + 4, "each bad release is counted as dropped");
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 1, "the lease is still live");

   bus_event_t ev;
   must_rc(bus_client_poll(&other, &ev), BUS_CLIENT_EMPTY, "releases are never routed");
   must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_OK, "the holder reads its frame");
   must(ev.payload != NULL, "and its payload");
   must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_EMPTY, "then returns the ref");
   bus_host_pump(&h);
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 0, "the real release drains it");

   bus_client_detach(&pub);
   bus_client_detach(&sub);
   bus_client_detach(&other);
   bus_host_destroy(&h);
   printf("  lease release: holder- and generation-checked, never routed\n");
}

static void test_request_reply(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");

   bus_client_t req, server;
   must_rc(attach_client(&h, &req), BUS_CLIENT_OK, "requester attaches");
   must_rc(attach_client(&h, &server), BUS_CLIENT_OK, "server attaches");
   must(bus_host_serve_kind(&h, server.reply.handle_id, KIND_A) == BUS_HOST_OK, "serve kind");

   const uint64_t corr = 0xBEEF;
   must_rc(bus_client_request(&req, KIND_A, corr, "ping", 4), BUS_CLIENT_OK, "send request");
   must(bus_host_pump(&h) == 1, "route request");

   bus_event_t ev;
   must_rc(bus_client_poll(&server, &ev), BUS_CLIENT_OK, "server gets request");
   must((ev.frame.hdr_flags & BUS_F_REQUEST), "is the request");
   must(ev.payload_len == 4 && memcmp(ev.payload, "ping", 4) == 0, "request payload");
   /* The server answers with the correlation the host handed it, which is
    * bus-unique rather than the requester's own; the host maps it back. */
   const uint64_t served = ev.frame.correlation_id;

   must_rc(bus_client_reply(&server, KIND_A, served, "pong", 4), BUS_CLIENT_OK, "server replies");
   must(bus_host_pump(&h) == 1, "route reply");

   must_rc(bus_client_poll(&req, &ev), BUS_CLIENT_OK, "requester gets reply");
   must(ev.frame.correlation_id == corr && (ev.frame.hdr_flags & BUS_F_REPLY), "is the reply");
   must(ev.payload_len == 4 && memcmp(ev.payload, "pong", 4) == 0, "reply payload");

   bus_client_detach(&req);
   bus_client_detach(&server);
   bus_host_destroy(&h);
   printf("  request/reply: round-trips through the client library\n");
}

static void test_capability_absent(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t req;
   must_rc(attach_client(&h, &req), BUS_CLIENT_OK, "attach");

   must_rc(bus_client_request(&req, KIND_A, 0x1234, NULL, 0), BUS_CLIENT_OK, "request no server");
   must(bus_host_pump(&h) == 1, "route");
   bus_event_t ev;
   must_rc(bus_client_poll(&req, &ev), BUS_CLIENT_OK, "poll");
   must(ev.frame.event_kind == BUS_KIND_CAPABILITY_ABSENT && ev.frame.correlation_id == 0x1234,
        "got capability_absent for the missing server");

   bus_client_detach(&req);
   bus_host_destroy(&h);
   printf("  capability_absent: surfaced to the requester\n");
}

static void test_would_block(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t pub;
   must_rc(attach_client(&h, &pub), BUS_CLIENT_OK, "attach");

   /* Fill the outbound ring without pumping the host: capacity publishes
    * succeed, then the next returns would_block rather than overwriting or
    * crashing. */
   uint32_t cap = pub.reply.queue_capacity;
   uint32_t ok = 0;
   for (uint32_t i = 0; i < cap + 4; i++)
   {
      bus_client_result_t r = bus_client_publish(&pub, KIND_A, "x", 1);
      if (r == BUS_CLIENT_OK)
         ok++;
      else
      {
         must_rc(r, BUS_CLIENT_WOULD_BLOCK, "full outbound returns would_block");
         break;
      }
   }
   must(ok == cap, "exactly capacity publishes succeeded before would_block");

   /* Draining via the host frees the ring, and publishing resumes. */
   bus_host_pump(&h);
   must_rc(bus_client_publish(&pub, KIND_A, "x", 1), BUS_CLIENT_OK, "publish resumes after drain");

   /* An oversize payload is refused up front, not truncated. */
   char big[512];
   memset(big, 'z', sizeof big);
   must_rc(bus_client_publish(&pub, KIND_A, big, cf.inline_budget + 1), BUS_CLIENT_ERR_PAYLOAD,
           "oversize inline payload refused");

   bus_client_detach(&pub);
   bus_host_destroy(&h);
   printf("  would_block: a full outbound ring is reported, not overrun\n");
}

static void test_heartbeat_and_epoch(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t c;
   must_rc(attach_client(&h, &c), BUS_CLIENT_OK, "attach");

   /* Heartbeat keeps the client from being reaped. */
   bus_client_heartbeat(&c, 1);
   must(bus_host_reap(&h, 100, 50) == 0, "baseline");
   bus_client_heartbeat(&c, 2);
   must(bus_host_reap(&h, 130, 50) == 0, "beating client not reaped");
   must(bus_host_admitted(&h) == 1, "still admitted");

   /* A host restart is observed. */
   must(!bus_client_epoch_changed(&c), "no restart yet");
   bus_host_bump_epoch(&h);
   must(bus_client_epoch_changed(&c), "client observes the host restart");
   /* And a publish after a restart refuses rather than writing a stale mapping. */
   must_rc(bus_client_publish(&c, KIND_A, "x", 1), BUS_CLIENT_EPOCH,
           "publish refused after restart");
   bus_event_t ev;
   must_rc(bus_client_poll(&c, &ev), BUS_CLIENT_EPOCH, "poll refused after restart");

   bus_client_detach(&c);
   bus_host_destroy(&h);
   printf("  heartbeat/epoch: kept alive; a restart is observed and fails closed\n");
}

/* Write a raw frame into a client's outbound ring, bypassing the client's own
 * checks, as a hostile or broken client could. */
static void inject(bus_client_t *c, const bus_frame_t *f, uint32_t slot_size)
{
   uint8_t *slot = bus_ring_produce_begin(&c->qp.outbound);
   must(slot != NULL, "outbound has room");
   must(bus_wire_encode(f, slot, slot_size) == BUS_WIRE_HDR_LEN, "frame encodes");
   bus_ring_produce_commit(&c->qp.outbound);
}

static int g_tapped;
static void count_tap(void *ctx, const bus_frame_t *f, const uint8_t *p, uint32_t n)
{
   (void)ctx;
   (void)f;
   (void)p;
   (void)n;
   g_tapped++;
}

/* An inline payload_ref is a client's claim. One near 2^64 wraps
 * payload_ref + payload_len past the slot bound, which once let any admitted
 * client make the host read outside the slot (a release, a tap) or write
 * outside a destination slot (a routed notification), or crash it. Ingress now
 * decodes with the placement check, so each is dropped before anything reads it. */
static void test_wrapped_payload_ref_is_dropped(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");
   bus_host_set_tap(&h, count_tap, NULL);
   bus_client_t pub, sub;
   must_rc(attach_client(&h, &pub), BUS_CLIENT_OK, "publisher attaches");
   must_rc(attach_client(&h, &sub), BUS_CLIENT_OK, "subscriber attaches");
   must(bus_host_subscribe(&h, sub.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");

   const struct
   {
      uint32_t kind;
      uint64_t back;
      uint32_t len;
   } cases[] = {
       {KIND_A, 64, 72},                 /* would write 72 bytes before the destination slot */
       {KIND_A, 200000, 200008},         /* would crash the pump */
       {BUS_KIND_LEASE_RELEASE, 8, 8},   /* would read 8 bytes before the ring slot */
       {KIND_A, 0, 8},                   /* payload_ref inside the header */
       {KIND_A, (uint64_t)0 - 200, 100}, /* payload_ref past the slot */
   };
   for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
   {
      bus_frame_t f;
      memset(&f, 0, sizeof f);
      f.hdr_flags = BUS_F_NOTIFICATION | BUS_F_INLINE;
      f.wire_version = BUS_WIRE_VERSION;
      f.event_kind = cases[i].kind;
      f.payload_ref = (uint64_t)0 - cases[i].back;
      f.payload_len = cases[i].len;
      inject(&pub, &f, cf.slot_size);
      uint64_t dropped = h.slots[pub.reply.handle_id].dropped;
      g_tapped = 0;
      bus_host_pump(&h);
      must(h.slots[pub.reply.handle_id].dropped == dropped + 1, "the frame is dropped");
      must(g_tapped == 0, "and never reaches the tap");
      bus_event_t ev;
      must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_EMPTY, "nothing is routed");
   }
   /* The host is intact: an ordinary notification still routes. */
   must_rc(bus_client_publish(&pub, KIND_A, "ok", 2), BUS_CLIENT_OK, "publish");
   must(bus_host_pump(&h) == 1, "routes");
   bus_event_t ev;
   must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_OK, "delivered");
   must(ev.payload_len == 2 && memcmp(ev.payload, "ok", 2) == 0, "intact");

   bus_client_detach(&pub);
   bus_client_detach(&sub);
   bus_host_destroy(&h);
   printf("  wrapped payload_ref: dropped at ingress, never read, written or tapped\n");
}

static uint32_t publish_arena_payload(bus_host_t *h, bus_client_t *pub, uint32_t kind, uint32_t len,
                                      uint8_t fill, uint32_t *generation)
{
   uint32_t lease = 0;
   must(bus_arena_alloc(&h->arena, pub->reply.handle_id, len, &lease) == BUS_ARENA_OK, "alloc");
   uint8_t *p = NULL;
   must(bus_arena_fill_ptr(&h->arena, lease, &p) == BUS_ARENA_OK, "fill");
   memset(p, fill, len);
   bus_arena_ref_t ref;
   must(bus_arena_ref(&h->arena, lease, &ref) == BUS_ARENA_OK, "ref");
   must_rc(bus_client_publish_arena(pub, kind, lease, ref.generation, len), BUS_CLIENT_OK,
           "publish arena");
   if (generation)
      *generation = ref.generation;
   return lease;
}

/* A blocked arena delivery is retried from the target set captured at first
 * sight. If that slot is reaped and a new client admitted into it, the
 * newcomer holds no reference: it must not be handed the frame, even though
 * the lease is still live for another consumer and would resolve. */
static void test_retry_never_delivers_to_a_reused_slot(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t filler, prod, sub, keeper, newcomer;
   must_rc(attach_client(&h, &filler), BUS_CLIENT_OK, "filler attaches");
   must_rc(attach_client(&h, &prod), BUS_CLIENT_OK, "producer attaches");
   must_rc(attach_client(&h, &sub), BUS_CLIENT_OK, "subscriber attaches");
   must_rc(attach_client(&h, &keeper), BUS_CLIENT_OK, "keeper attaches");
   must(bus_host_subscribe(&h, sub.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe sub");
   must(bus_host_subscribe(&h, keeper.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe keeper");
   must(bus_host_subscribe(&h, sub.reply.handle_id, KIND_A + 1) == BUS_HOST_OK,
        "sub alone takes the filler kind");

   /* Fill sub's inbound with a kind only it observes. */
   for (uint32_t i = 0; i < cf.queue_capacity * 2; i++)
   {
      if (bus_client_publish(&filler, KIND_A + 1, "f", 1) != BUS_CLIENT_OK)
         break;
      bus_host_pump(&h);
   }
   uint32_t gen = 0;
   uint32_t lease = publish_arena_payload(&h, &prod, KIND_A, 700, 0x5a, &gen);
   bus_host_pump(&h);
   must(bus_arena_holds(&h.arena, lease, gen, keeper.reply.handle_id), "keeper holds a ref");
   must(bus_arena_holds(&h.arena, lease, gen, sub.reply.handle_id), "so does the blocked sub");

   uint32_t sub_slot = sub.reply.handle_id;
   must(bus_host_release_slot(&h, sub_slot) == BUS_HOST_OK, "sub's slot is released");
   bus_client_detach(&sub);
   must(!bus_arena_holds(&h.arena, lease, gen, sub_slot), "its reference went with it");
   must_rc(attach_client(&h, &newcomer), BUS_CLIENT_OK, "a newcomer attaches");
   must(newcomer.reply.handle_id == sub_slot, "into the same slot");

   for (int i = 0; i < 4; i++)
      bus_host_pump(&h);
   bus_event_t ev;
   must_rc(bus_client_poll(&newcomer, &ev), BUS_CLIENT_EMPTY,
           "the newcomer is never handed a frame it holds no reference for");

   must_rc(bus_client_poll(&keeper, &ev), BUS_CLIENT_OK, "the keeper reads it");
   must(ev.payload && ev.payload_len == 700 && ev.payload[699] == 0x5a, "intact");
   must_rc(bus_client_poll(&keeper, &ev), BUS_CLIENT_EMPTY, "and returns it");
   bus_host_pump(&h);
   must(bus_arena_bytes_in_use(&h.arena) == 0, "the lease drained");

   bus_client_detach(&filler);
   bus_client_detach(&prod);
   bus_client_detach(&keeper);
   bus_client_detach(&newcomer);
   bus_host_destroy(&h);
   printf("  arena retry: delivery needs the reference, not just an occupied slot\n");
}

/* A malformed release placed in the arena still names a lease its sender
 * produced; it is reclaimed, by its producer only, rather than stranded. */
static void test_arena_placed_release_reclaims_only_its_own(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t prod, other;
   must_rc(attach_client(&h, &prod), BUS_CLIENT_OK, "producer attaches");
   must_rc(attach_client(&h, &other), BUS_CLIENT_OK, "bystander attaches");
   uint32_t lease = 0;
   must(bus_arena_alloc(&h.arena, prod.reply.handle_id, 300, &lease) == BUS_ARENA_OK, "alloc");
   bus_arena_ref_t ref;
   must(bus_arena_ref(&h.arena, lease, &ref) == BUS_ARENA_OK, "ref");

   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.hdr_flags = BUS_F_NOTIFICATION | BUS_F_ARENA;
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = BUS_KIND_LEASE_RELEASE;
   f.payload_ref = lease;
   f.payload_len = 300;
   f.generation = ref.generation;
   inject(&other, &f, cf.slot_size);
   bus_host_pump(&h);
   must(bus_arena_live_leases(&h.arena, prod.reply.handle_id) == 1,
        "a bystander cannot reclaim another producer's lease");
   must(h.slots[other.reply.handle_id].dropped == 1, "its frame is dropped");

   inject(&prod, &f, cf.slot_size);
   bus_host_pump(&h);
   must(bus_arena_live_leases(&h.arena, prod.reply.handle_id) == 0, "its producer's is reclaimed");
   must(h.slots[prod.reply.handle_id].dropped == 1, "and still counted as malformed");

   bus_client_detach(&prod);
   bus_client_detach(&other);
   bus_host_destroy(&h);
   printf("  arena-placed release: reclaimed by its producer only, never stranded\n");
}

/* A governed module process has grants on what it may inject. Returning a
 * reference is not an injection, so a slot with no outbound grants at all must
 * still release what it was given. */
static void test_release_needs_no_grant(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t pub, sub;
   must_rc(attach_client(&h, &pub), BUS_CLIENT_OK, "publisher attaches");
   must_rc(attach_client(&h, &sub), BUS_CLIENT_OK, "subscriber attaches");
   must(bus_host_subscribe(&h, sub.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");
   must(bus_host_enforce_grants(&h, sub.reply.handle_id) == BUS_HOST_OK, "sub is governed");

   must_rc(bus_client_publish(&sub, KIND_A, "x", 1), BUS_CLIENT_OK, "an ungranted publish");
   bus_host_pump(&h);
   must(h.slots[sub.reply.handle_id].dropped == 1, "is refused (grants are enforced)");

   publish_arena_payload(&h, &pub, KIND_A, 400, 0x11, NULL);
   bus_host_pump(&h);
   bus_event_t ev;
   while (bus_client_poll(&sub, &ev) == BUS_CLIENT_OK && !(ev.frame.hdr_flags & BUS_F_ARENA))
      ; /* skip the capability_denied control frame */
   must(ev.frame.hdr_flags & BUS_F_ARENA, "the governed slot receives the arena frame");
   must(ev.payload && ev.payload[0] == 0x11, "and reads it");
   must_rc(bus_client_poll(&sub, &ev), BUS_CLIENT_EMPTY, "then returns it");
   bus_host_pump(&h);
   must(h.slots[sub.reply.handle_id].dropped == 1, "the release is not refused");
   must(bus_arena_bytes_in_use(&h.arena) == 0, "the lease drained");

   bus_client_detach(&pub);
   bus_client_detach(&sub);
   bus_host_destroy(&h);
   printf("  release: accepted from a governed slot with no grants\n");
}

/* With its outbound ring full, a client cannot return refs; it queues up to
 * BUS_CLIENT_MAX_PENDING_RELEASES and then stops taking arena frames, so
 * backpressure reaches the host. When the ring drains, everything resumes and
 * every reference goes back. */
static void test_release_backlog(void)
{
   bus_host_config_t cf = cfg();
   cf.queue_capacity = 128; /* room for the backlog's arena frames, and the producer's cap */
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");
   bus_client_t pub, sub;
   must_rc(attach_client(&h, &pub), BUS_CLIENT_OK, "publisher attaches");
   must_rc(attach_client(&h, &sub), BUS_CLIENT_OK, "subscriber attaches");
   must(bus_host_subscribe(&h, sub.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");

   const uint32_t n = BUS_CLIENT_MAX_PENDING_RELEASES + 10;
   for (uint32_t i = 0; i < n; i++)
   {
      publish_arena_payload(&h, &pub, KIND_A, 300, (uint8_t)i, NULL);
      bus_host_pump(&h);
   }
   must(bus_ring_count(&sub.qp.inbound) == n, "every arena frame is queued for sub");

   /* Fill sub's outbound so no release can leave. */
   uint32_t filled = 0;
   while (bus_client_publish(&sub, KIND_A + 7, "z", 1) == BUS_CLIENT_OK)
      filled++;
   must(filled == cf.queue_capacity, "outbound full");

   uint32_t got = 0;
   bus_event_t ev;
   while (bus_client_poll(&sub, &ev) == BUS_CLIENT_OK)
   {
      must(ev.payload && ev.payload[0] == (uint8_t)got, "in order, readable");
      got++;
   }
   must(sub.release_count == BUS_CLIENT_MAX_PENDING_RELEASES, "the backlog is full");
   must(got == BUS_CLIENT_MAX_PENDING_RELEASES, "one frame read per queued ref, then none");
   must(bus_ring_count(&sub.qp.inbound) == n - got, "the rest wait unread");
   must_rc(bus_client_flush_releases(&sub), BUS_CLIENT_WOULD_BLOCK, "flush cannot send yet");

   bus_host_pump(&h); /* drains sub's outbound */
   while (bus_client_poll(&sub, &ev) == BUS_CLIENT_OK)
   {
      must(ev.payload && ev.payload[0] == (uint8_t)got, "resumes in order");
      got++;
   }
   must(got == n, "every frame was read");
   must_rc(bus_client_flush_releases(&sub), BUS_CLIENT_OK, "nothing left owed");
   bus_host_pump(&h);
   must(bus_arena_bytes_in_use(&h.arena) == 0, "every reference went back");
   must(h.slots[sub.reply.handle_id].dropped == 0, "none was refused");

   bus_client_detach(&pub);
   bus_client_detach(&sub);
   bus_host_destroy(&h);
   printf("  release backlog: bounded, applies backpressure, drains completely\n");
}

int main(void)
{
   printf("test_bus_client:\n");
   test_wrapped_payload_ref_is_dropped();
   test_retry_never_delivers_to_a_reused_slot();
   test_arena_placed_release_reclaims_only_its_own();
   test_release_needs_no_grant();
   test_release_backlog();
   test_publish_arena();
   test_release_is_holder_checked();
   test_publish_and_poll();
   test_request_reply();
   test_capability_absent();
   test_would_block();
   test_heartbeat_and_epoch();
   printf("test_bus_client: OK\n");
   return 0;
}
