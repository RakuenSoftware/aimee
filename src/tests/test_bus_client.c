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
 * host routes it by reference; the subscriber polls the frame and reads the
 * payload in place via the lease table, then releases it. */
static void test_publish_arena(void)
{
   bus_host_config_t cf = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &cf, NULL, NULL) == BUS_HOST_OK, "host");

   bus_client_t pub, sub;
   must_rc(attach_client(&h, &pub), BUS_CLIENT_OK, "publisher attaches");
   must_rc(attach_client(&h, &sub), BUS_CLIENT_OK, "subscriber attaches");
   must(bus_host_subscribe(&h, sub.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");

   /* A zero-length arena payload is refused: there is nothing to lease. */
   must_rc(bus_client_publish_arena(&pub, KIND_A, 0, 0, 0), BUS_CLIENT_ERR_PAYLOAD,
           "empty refused");

   const uint32_t len = cf.inline_budget + 500; /* too big for inline: genuinely arena */
   uint32_t lease = 0;
   must(bus_arena_alloc(&h.arena, pub.reply.handle_id, len, &lease) == BUS_ARENA_OK, "alloc lease");
   uint8_t *p = NULL;
   must(bus_arena_fill_ptr(&h.arena, lease, &p) == BUS_ARENA_OK, "fill ptr");
   memset(p, 0xD9, len);
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
   must(ev.payload == NULL, "poll does not auto-resolve arena bytes (the lease table does)");

   const uint8_t *rp = NULL;
   must(bus_arena_read_ptr(&h.arena, lease, ev.frame.generation, sub.reply.handle_id, &rp) ==
            BUS_ARENA_OK,
        "consumer reads the lease in place");
   must(rp[0] == 0xD9 && rp[len - 1] == 0xD9, "arena payload matches");
   must(bus_arena_release(&h.arena, lease, ev.frame.generation, sub.reply.handle_id) ==
            BUS_ARENA_OK,
        "release");
   must(bus_arena_live_leases(&h.arena, pub.reply.handle_id) == 0, "lease drained after release");

   bus_client_detach(&pub);
   bus_client_detach(&sub);
   bus_host_destroy(&h);
   printf("  publish_arena: a large payload routes by lease reference, read in place\n");
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

int main(void)
{
   printf("test_bus_client:\n");
   test_publish_arena();
   test_publish_and_poll();
   test_request_reply();
   test_capability_absent();
   test_would_block();
   test_heartbeat_and_epoch();
   printf("test_bus_client: OK\n");
   return 0;
}
