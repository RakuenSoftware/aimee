/* test_bus_capture.c: slice 11 of the event-bus feature tree.
 *
 * The capture stream and observational replay (D10):
 *
 *   - end to end: the tap records a host's event stream; reading it back yields
 *     the same events, in contiguous seq order, with materialized payloads, and
 *     observational replay reproduces them exactly;
 *   - the terminal-states parser is total and decides open / complete /
 *     truncated / corrupt from the bytes alone — a clean tail is open, an
 *     epoch_change tail is complete, a torn final record is truncated, and a
 *     seq gap, a mid-stream CRC failure, a record after epoch_change, or an
 *     unknown format_version are all corrupt.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <aimee/core/event_bus/bus_capture.h>
#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_wire.h>

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

/* ---- replay collector ---- */

#define MAXREC 256
static uint64_t g_seq[MAXREC];
static uint32_t g_kind[MAXREC];
static uint32_t g_n;
static void collect(void *ctx, const bus_capture_event_t *ev)
{
   (void)ctx;
   must(g_n < MAXREC, "replay buffer");
   g_seq[g_n] = ev->frame.seq;
   g_kind[g_n] = ev->frame.event_kind;
   g_n++;
}

/* ---- a directly-driven tap, for precise stream construction ---- */

static void tap_frame(bus_capture_t *c, uint32_t kind, uint64_t seq, const char *payload)
{
   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = kind;
   f.hdr_flags = BUS_F_NOTIFICATION;
   f.seq = seq;
   uint32_t plen = payload ? (uint32_t)strlen(payload) : 0;
   if (plen)
   {
      f.hdr_flags |= BUS_F_INLINE;
      f.payload_len = plen;
      f.payload_ref = BUS_WIRE_HDR_LEN;
   }
   bus_capture_tap(c, &f, (const uint8_t *)payload, plen);
}

/* ------------------------------------------------------------------ */

static void test_roundtrip_and_replay(void)
{
   bus_capture_t cap;
   bus_capture_init(&cap, 1, 1, 7);

   const char *msgs[] = {"alpha", "bravo", "charlie", "delta"};
   for (int i = 0; i < 4; i++)
      tap_frame(&cap, 300 + i, (uint64_t)(100 + i), msgs[i]);

   g_n = 0;
   bus_capture_report_t rep = bus_capture_read(cap.buf, cap.len, collect, NULL);
   must(rep.status == BUS_CAPTURE_OPEN, "a clean, non-terminated stream is open");
   must(rep.records == 4, "all four records parsed");
   must(g_n == 4, "replay produced four events");
   for (int i = 0; i < 4; i++)
   {
      must(g_seq[i] == (uint64_t)(100 + i), "seq preserved in order");
      must(g_kind[i] == (uint32_t)(300 + i), "kind preserved");
   }
   /* Materialized payloads survive: re-read and check the bytes of one. */
   must(rep.last_good_seq == 103, "last good seq");

   bus_capture_free(&cap);
   printf("  roundtrip: capture -> read -> replay reproduces the stream in order\n");
}

/* Verify the materialized payload bytes come back exactly. */
static const char *g_want_payload;
static int g_payload_ok;
static void check_payload(void *ctx, const bus_capture_event_t *ev)
{
   (void)ctx;
   if (ev->frame.seq == 500)
   {
      g_payload_ok = ev->payload_len == (uint32_t)strlen(g_want_payload) &&
                     memcmp(ev->payload, g_want_payload, ev->payload_len) == 0;
   }
}

static void test_materialized_payload(void)
{
   bus_capture_t cap;
   bus_capture_init(&cap, 1, 1, 1);
   g_want_payload = "the-payload-bytes";
   tap_frame(&cap, 400, 500, g_want_payload);
   g_payload_ok = 0;
   bus_capture_read(cap.buf, cap.len, check_payload, NULL);
   must(g_payload_ok, "materialized payload bytes come back exactly");
   bus_capture_free(&cap);
   printf("  materialized payload: recorded and read back byte-exact\n");
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
static void attach_client(bus_host_t *h, bus_client_t *c)
{
   int sv[2];
   must(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");
   struct serve_arg a = {.h = h, .fd = sv[1]};
   pthread_t t;
   must(pthread_create(&t, NULL, serve_thread, &a) == 0, "spawn serve");
   must(bus_client_attach(sv[0], c) == BUS_CLIENT_OK, "attach");
   pthread_join(t, NULL);
   close(sv[0]);
   close(sv[1]);
}

/* Route real events through the host with the capture tap installed, and confirm
 * the captured stream is exactly what was routed. */
static uint32_t g_e2e_kinds[8];
static uint32_t g_e2e_n;
static void e2e_collect(void *ctx, const bus_capture_event_t *ev)
{
   (void)ctx;
   if (g_e2e_n < 8)
      g_e2e_kinds[g_e2e_n++] = ev->frame.event_kind;
}

static void test_end_to_end_host(void)
{
   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 4;
   cfg.slot_size = 256;
   cfg.inline_budget = 192;
   cfg.queue_capacity = 16;
   cfg.arena_size = 128 * 1024;

   bus_host_t h;
   must(bus_host_create(&h, &cfg, NULL, NULL) == BUS_HOST_OK, "host");
   bus_capture_t cap;
   bus_capture_init(&cap, 1, 1, bus_control_epoch(h.control));
   bus_host_set_tap(&h, bus_capture_tap, &cap);

   bus_client_t pub, sub;
   attach_client(&h, &pub);
   attach_client(&h, &sub);
   must(bus_host_subscribe(&h, sub.reply.handle_id, 700) == BUS_HOST_OK, "subscribe");

   /* Two real publishes routed through the host; the tap records each once. */
   must(bus_client_publish(&pub, 700, "one", 3) == BUS_CLIENT_OK, "publish 1");
   must(bus_client_publish(&pub, 700, "two", 3) == BUS_CLIENT_OK, "publish 2");
   must(bus_host_pump(&h) == 2, "host routes both");

   /* The subscriber actually received them (delivered stream), and the capture
    * recorded them (intended stream) — the two derivable from one another (D6). */
   bus_event_t ev;
   must(bus_client_poll(&sub, &ev) == BUS_CLIENT_OK && ev.frame.event_kind == 700, "delivered 1");
   must(bus_client_poll(&sub, &ev) == BUS_CLIENT_OK && ev.frame.event_kind == 700, "delivered 2");

   g_e2e_n = 0;
   bus_capture_report_t rep = bus_capture_read(cap.buf, cap.len, e2e_collect, NULL);
   must(rep.status == BUS_CAPTURE_OPEN && rep.records == 2, "capture recorded both routed events");
   must(g_e2e_n == 2 && g_e2e_kinds[0] == 700 && g_e2e_kinds[1] == 700,
        "replay reproduces the routed kinds in order");

   bus_client_detach(&pub);
   bus_client_detach(&sub);
   bus_capture_free(&cap);
   bus_host_destroy(&h);
   printf("  end-to-end: real routed events are captured and replayed in order\n");
}

static void test_terminal_states(void)
{
   /* complete: a stream ending in an epoch_change record. */
   {
      bus_capture_t c;
      bus_capture_init(&c, 1, 1, 1);
      tap_frame(&c, 800, 10, "x");
      tap_frame(&c, BUS_KIND_EPOCH_CHANGE, 11, NULL);
      bus_capture_report_t r = bus_capture_read(c.buf, c.len, NULL, NULL);
      must(r.status == BUS_CAPTURE_COMPLETE, "epoch_change tail -> complete");
      bus_capture_free(&c);
   }

   /* corrupt: a record after epoch_change. */
   {
      bus_capture_t c;
      bus_capture_init(&c, 1, 1, 1);
      tap_frame(&c, BUS_KIND_EPOCH_CHANGE, 20, NULL);
      tap_frame(&c, 800, 21, "x");
      bus_capture_report_t r = bus_capture_read(c.buf, c.len, NULL, NULL);
      must(r.status == BUS_CAPTURE_CORRUPT, "record after epoch_change -> corrupt");
      bus_capture_free(&c);
   }

   /* corrupt: a seq gap. */
   {
      bus_capture_t c;
      bus_capture_init(&c, 1, 1, 1);
      tap_frame(&c, 800, 30, "a");
      tap_frame(&c, 801, 32, "b"); /* 31 skipped */
      bus_capture_report_t r = bus_capture_read(c.buf, c.len, NULL, NULL);
      must(r.status == BUS_CAPTURE_CORRUPT && r.rule == 6, "seq gap -> corrupt");
      bus_capture_free(&c);
   }

   /* truncated: chop the last record mid-body. */
   {
      bus_capture_t c;
      bus_capture_init(&c, 1, 1, 1);
      tap_frame(&c, 800, 40, "hello");
      tap_frame(&c, 801, 41, "world");
      size_t chopped = c.len - 3; /* lose the last 3 bytes of the final record */
      bus_capture_report_t r = bus_capture_read(c.buf, chopped, NULL, NULL);
      must(r.status == BUS_CAPTURE_TRUNCATED, "torn final record -> truncated");
      must(r.records == 1 && r.last_good_seq == 40, "the first record survived");
      bus_capture_free(&c);
   }

   /* corrupt: a mid-stream CRC failure (bytes follow it). */
   {
      bus_capture_t c;
      bus_capture_init(&c, 1, 1, 1);
      tap_frame(&c, 800, 50, "aaaa");
      tap_frame(&c, 801, 51, "bbbb");
      /* flip a byte inside the first record's payload; its CRC now fails and it
       * is not the last record. */
      c.buf[BUS_CAPTURE_HEADER_LEN + 8 + BUS_WIRE_HDR_LEN] ^= 0xff;
      bus_capture_report_t r = bus_capture_read(c.buf, c.len, NULL, NULL);
      must(r.status == BUS_CAPTURE_CORRUPT && r.rule == 5, "mid-stream CRC failure -> corrupt");
      bus_capture_free(&c);
   }

   /* corrupt: an unknown format_version is refused before any record. */
   {
      bus_capture_t c;
      bus_capture_init(&c, 1, 1, 1);
      tap_frame(&c, 800, 60, "x");
      c.buf[8] = 0x7f; /* format_version */
      bus_capture_report_t r = bus_capture_read(c.buf, c.len, NULL, NULL);
      must(r.status == BUS_CAPTURE_CORRUPT && r.rule == 0, "unknown format_version -> corrupt");
      bus_capture_free(&c);
   }

   printf("  terminal states: open/complete/truncated/corrupt decided from bytes\n");
}

/* An arena event is captured too. The host resolves the producer-held span
 * pre-routing and hands the bytes to the tap, so the record materializes them and
 * replay reproduces them from the record blob — never the long-gone lease. */
static uint8_t g_arena_seen[4096];
static uint32_t g_arena_seen_len;
static int g_arena_materialized;
static void arena_collect(void *ctx, const bus_capture_event_t *ev)
{
   (void)ctx;
   if (!(ev->frame.hdr_flags & BUS_F_ARENA))
      return;
   g_arena_materialized = ev->payload_len > 0 && ev->payload != NULL;
   g_arena_seen_len = ev->payload_len;
   if (ev->payload && ev->payload_len <= sizeof g_arena_seen)
      memcpy(g_arena_seen, ev->payload, ev->payload_len);
}

static void test_arena_captured_and_replayed(void)
{
   bus_host_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.max_slots = 4;
   cfg.slot_size = 256;
   cfg.inline_budget = 192;
   cfg.queue_capacity = 16;
   cfg.arena_size = 128 * 1024;

   bus_host_t h;
   must(bus_host_create(&h, &cfg, NULL, NULL) == BUS_HOST_OK, "host");
   bus_capture_t cap;
   bus_capture_init(&cap, 1, 1, bus_control_epoch(h.control));
   bus_host_set_tap(&h, bus_capture_tap, &cap);

   bus_client_t pub, sub;
   attach_client(&h, &pub);
   attach_client(&h, &sub);
   must(bus_host_subscribe(&h, sub.reply.handle_id, 700) == BUS_HOST_OK, "subscribe");

   /* An arena-sized payload (> inline budget) with a known per-byte pattern. */
   const uint32_t len = 1000;
   uint32_t lease = 0;
   must(bus_arena_alloc(&h.arena, pub.reply.handle_id, len, &lease) == BUS_ARENA_OK, "alloc");
   uint8_t *fp = NULL;
   must(bus_arena_fill_ptr(&h.arena, lease, &fp) == BUS_ARENA_OK, "fill");
   for (uint32_t i = 0; i < len; i++)
      fp[i] = (uint8_t)(i * 7 + 1);
   bus_arena_ref_t ref;
   must(bus_arena_ref(&h.arena, lease, &ref) == BUS_ARENA_OK, "ref");
   must(bus_client_publish_arena(&pub, 700, lease, ref.generation, len) == BUS_CLIENT_OK,
        "publish");
   must(bus_host_pump(&h) == 1, "host routes the arena event");

   /* Routing still works alongside capture: the subscriber reads and releases. */
   bus_event_t ev;
   must(bus_client_poll(&sub, &ev) == BUS_CLIENT_OK && (ev.frame.hdr_flags & BUS_F_ARENA),
        "arena event delivered");
   const uint8_t *rp = NULL;
   must(bus_arena_read_ptr(&h.arena, lease, ev.frame.generation, sub.reply.handle_id, &rp) ==
            BUS_ARENA_OK,
        "consumer reads in place");
   must(bus_arena_release(&h.arena, lease, ev.frame.generation, sub.reply.handle_id) ==
            BUS_ARENA_OK,
        "consumer releases");

   /* The capture materialized the arena bytes; replay returns them byte-exact. */
   g_arena_materialized = 0;
   g_arena_seen_len = 0;
   bus_capture_report_t rep = bus_capture_read(cap.buf, cap.len, arena_collect, NULL);
   must(rep.status == BUS_CAPTURE_OPEN && rep.records == 1, "arena event captured");
   must(g_arena_materialized && g_arena_seen_len == len,
        "arena payload materialized in the record");
   int ok = 1;
   for (uint32_t i = 0; i < len; i++)
      if (g_arena_seen[i] != (uint8_t)(i * 7 + 1))
         ok = 0;
   must(ok, "replayed arena bytes match what the producer filled");

   bus_client_detach(&pub);
   bus_client_detach(&sub);
   bus_capture_free(&cap);
   bus_host_destroy(&h);
   printf("  arena capture: an arena payload is materialized and replayed byte-exact\n");
}

int main(void)
{
   printf("test_bus_capture:\n");
   test_roundtrip_and_replay();
   test_materialized_payload();
   test_end_to_end_host();
   test_arena_captured_and_replayed();
   test_terminal_states();
   printf("test_bus_capture: OK\n");
   return 0;
}
