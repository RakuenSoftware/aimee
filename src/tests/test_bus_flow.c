/* test_bus_flow.c: slice 7 of the event-bus feature tree — flow control.
 *
 * The properties that make backpressure safe, checked as outcomes:
 *
 *   - A BLOCK-policy event to a full destination is not lost: it is held at the
 *     producer's ring head and delivered once the destination drains. It is
 *     seq-stamped and tapped exactly once across the retries.
 *   - One slow consumer stalls only its own producer, never another.
 *   - A SHED-policy event to a full destination produces a typed overflow event
 *     naming the lost seq and kind — a consumer can enumerate its losses.
 *   - Overflow (a control-class event) is delivered from a reserve even when the
 *     data ring is full; when the reserve too is exhausted, a sticky
 *     control_lost flag is set — never a silent loss.
 *   - Reaping a producer that had a block-held event names the discarded seq to
 *     the tap as producer_reaped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <aimee/audit/audit_worm.h>
#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_ring.h>
#include <aimee/core/event_bus/bus_wire.h>

#include "cJSON.h"
#include "platform_test_util.h"

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

static void emit(client_t *c, uint16_t flags, uint32_t kind, uint32_t plen, uint8_t fill)
{
   uint8_t *slot = bus_ring_produce_begin(&c->qp.outbound);
   must(slot != NULL, "outbound has room");
   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.hdr_flags = flags | (plen ? BUS_F_INLINE : 0);
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = kind;
   f.payload_len = plen;
   if (plen)
      f.payload_ref = BUS_WIRE_HDR_LEN;
   must(bus_wire_encode(&f, slot, c->reply.slot_size) == BUS_WIRE_HDR_LEN, "encode");
   if (plen)
      memset(slot + BUS_WIRE_HDR_LEN, fill, plen);
   bus_ring_produce_commit(&c->qp.outbound);
}

static int recv_event(client_t *c, bus_frame_t *out, uint8_t *payload, uint32_t cap)
{
   const uint8_t *slot = bus_ring_consume_begin(&c->qp.inbound);
   if (!slot)
      return 0;
   must(bus_wire_decode(slot, c->reply.slot_size, out) == BUS_WIRE_OK, "decode inbound");
   if ((out->hdr_flags & BUS_F_INLINE) && out->payload_len > 0 && payload &&
       out->payload_len <= cap)
      memcpy(payload, slot + out->payload_ref, out->payload_len);
   bus_ring_consume_commit(&c->qp.inbound);
   return 1;
}

static bus_host_config_t cfg(void)
{
   bus_host_config_t c;
   memset(&c, 0, sizeof c);
   c.max_slots = 4;
   c.slot_size = 128;
   c.inline_budget = 64;
   c.queue_capacity = 8; /* small, with a control reserve of 4 -> 4 data slots */
   c.arena_size = 64 * 1024;
   return c;
}

/* ---- tap recorder ---- */

static uint64_t g_seq[512];
static uint32_t g_kind[512];
static uint32_t g_n;
static uint32_t g_loss_n;
static bus_frame_t g_loss_frame;
static uint8_t g_loss_payload[64];
static uint32_t g_loss_payload_len;
static void tap(void *ctx, const bus_frame_t *f, const uint8_t *pl, uint32_t pn)
{
   (void)pl;
   (void)pn;
   (void)ctx;
   must(g_n < 512, "tap buffer");
   g_seq[g_n] = f->seq;
   g_kind[g_n] = f->event_kind;
   g_n++;
}
static void loss_sink(void *ctx, const bus_frame_t *f, const uint8_t *pl, uint32_t pn)
{
   (void)ctx;
   must(pn <= sizeof g_loss_payload, "loss payload fits recorder");
   g_loss_frame = *f;
   g_loss_payload_len = pn;
   if (pn)
      memcpy(g_loss_payload, pl, pn);
   g_loss_n++;
   obs_bus_record_loss(NULL, f, pl, pn);
}

static int worm_count(const char *action)
{
   long total = 0;
   cJSON *rows = audit_worm_read_page(0, 256, &total);
   must(rows != NULL && total >= 0, "read WORM loss rows");
   int count = 0;
   const cJSON *row = NULL;
   cJSON_ArrayForEach(row, rows)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, "action");
      if (cJSON_IsString(value) && strcmp(value->valuestring, action) == 0)
         count++;
   }
   cJSON_Delete(rows);
   return count;
}

static int worm_sink(const char *role, const char *principal, const char *action,
                     const char *subject, const char *verdict, const char *detail, void *ctx)
{
   (void)ctx;
   return audit_worm_append(role, principal, action, subject, verdict, detail);
}
static uint32_t tap_count_kind(uint32_t kind)
{
   uint32_t n = 0;
   for (uint32_t i = 0; i < g_n; i++)
      if (g_kind[i] == kind)
         n++;
   return n;
}

#define KIND_A 400

/* The inbound data limit is capacity - control_reserve. */
static uint32_t data_limit(const client_t *c)
{
   uint32_t reserve = atomic_load_explicit(&c->qp.hdr->control_credits, memory_order_relaxed);
   return c->reply.queue_capacity - reserve;
}

static void test_block_holds_then_delivers(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");
   g_n = 0;
   g_loss_n = 0;
   bus_host_set_tap(&h, tap, NULL);
   bus_host_set_loss_sink(&h, loss_sink, NULL);

   client_t pub, obs;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs);
   must(bus_host_subscribe(&h, obs.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");
   /* KIND_A defaults to BLOCK. */

   uint32_t lim = data_limit(&obs);
   /* Emit one more than the data limit; the last one must block, not be lost. */
   for (uint32_t i = 0; i <= lim; i++)
      emit(&pub, BUS_F_NOTIFICATION, KIND_A, 4, (uint8_t)i);

   /* First pump: lim events delivered, the (lim+1)th blocks at the producer. */
   bus_host_pump(&h);
   must(h.slots[pub.reply.handle_id].blocked, "producer is blocked on the full destination");
   uint32_t tap_after_first = tap_count_kind(KIND_A);
   must(tap_after_first == lim + 1, "the blocked event was tapped once already");

   /* The consumer drains one slot; next pump delivers the held event. */
   bus_frame_t f;
   uint8_t buf[8];
   must(recv_event(&obs, &f, buf, sizeof buf) == 1, "consumer drains one");
   bus_host_pump(&h);
   must(!h.slots[pub.reply.handle_id].blocked, "producer unblocked once room appeared");

   /* Drain everything and count: exactly lim+1 distinct events reached the
    * consumer, none lost, none duplicated. */
   uint32_t got = 1; /* already drained one above */
   while (recv_event(&obs, &f, buf, sizeof buf))
      got++;
   must(got == lim + 1, "every blocked event was delivered exactly once");
   /* And the tap still saw it exactly once (no re-tap across retries). */
   must(tap_count_kind(KIND_A) == lim + 1, "no event tapped twice across retries");

   detach(&pub);
   detach(&obs);
   bus_host_destroy(&h);
   printf("  block: a full destination holds the event, then delivers it once\n");
}

static void test_block_is_per_producer(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t slow_pub, fast_pub, slow_obs, fast_obs;
   attach(&h, 1, &slow_pub);
   attach(&h, 2, &fast_pub);
   attach(&h, 3, &slow_obs);
   attach(&h, 4, &fast_obs);
   must(bus_host_subscribe(&h, slow_obs.reply.handle_id, KIND_A) == BUS_HOST_OK, "slow subs");
   must(bus_host_subscribe(&h, fast_obs.reply.handle_id, KIND_A + 1) == BUS_HOST_OK, "fast subs");

   /* Overfill the slow observer's kind so slow_pub blocks; fast_pub targets a
    * different kind/consumer that has room. */
   uint32_t lim = data_limit(&slow_obs);
   for (uint32_t i = 0; i <= lim; i++)
      emit(&slow_pub, BUS_F_NOTIFICATION, KIND_A, 4, 0);
   emit(&fast_pub, BUS_F_NOTIFICATION, KIND_A + 1, 4, 0);

   bus_host_pump(&h);
   must(h.slots[slow_pub.reply.handle_id].blocked, "slow producer blocked");
   must(!h.slots[fast_pub.reply.handle_id].blocked, "fast producer not blocked");

   /* The fast observer got its event despite the slow producer being stuck. */
   bus_frame_t f;
   must(recv_event(&fast_obs, &f, NULL, 0) == 1, "fast consumer served while slow is blocked");

   detach(&slow_pub);
   detach(&fast_pub);
   detach(&slow_obs);
   detach(&fast_obs);
   bus_host_destroy(&h);
   printf("  block: one slow consumer stalls only its own producer\n");
}

/* A block-policy notification to two observers, one full and one with room: the
 * observer with room is served immediately and exactly once — it must not be
 * re-delivered when the blocked observer later drains. This exercises the
 * per-retry delivered-bitmap, the subtle part of block fan-out. */
static void test_block_fanout_no_double_deliver(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t pub, full_obs, roomy_obs;
   attach(&h, 1, &pub);
   attach(&h, 2, &full_obs);
   attach(&h, 3, &roomy_obs);
   must(bus_host_subscribe(&h, full_obs.reply.handle_id, KIND_A) == BUS_HOST_OK, "full subs");
   must(bus_host_subscribe(&h, roomy_obs.reply.handle_id, KIND_A) == BUS_HOST_OK, "roomy subs");

   /* Fill full_obs to its data limit with prior events, then emit one more that
    * fans out to both. full_obs blocks; roomy_obs must get it once. */
   uint32_t lim = data_limit(&full_obs);
   for (uint32_t i = 0; i < lim; i++)
   {
      emit(&pub, BUS_F_NOTIFICATION, KIND_A, 4, 0);
      bus_host_pump(&h);
      /* roomy_obs also receives these; drain it so only the final event matters. */
      bus_frame_t tmp;
      recv_event(&roomy_obs, &tmp, NULL, 0);
   }
   /* full_obs is now at its data limit; roomy_obs is empty. */
   emit(&pub, BUS_F_NOTIFICATION, KIND_A, 4, 0x77);
   bus_host_pump(&h);
   must(h.slots[pub.reply.handle_id].blocked, "producer blocked on the full observer");

   /* roomy_obs got the fan-out event exactly once already. */
   bus_frame_t f;
   uint8_t buf[8];
   must(recv_event(&roomy_obs, &f, buf, sizeof buf) == 1 && buf[0] == 0x77,
        "roomy observer got the event");
   must(recv_event(&roomy_obs, &f, buf, sizeof buf) == 0, "roomy observer got it only once");

   /* Drain full_obs; the retry delivers the held copy to it, and must NOT
    * re-deliver to roomy_obs. */
   while (recv_event(&full_obs, &f, buf, sizeof buf))
      ; /* drain all */
   bus_host_pump(&h);
   must(!h.slots[pub.reply.handle_id].blocked, "producer unblocked");
   must(recv_event(&full_obs, &f, buf, sizeof buf) == 1 && buf[0] == 0x77,
        "full observer finally got the held event");
   must(recv_event(&roomy_obs, &f, buf, sizeof buf) == 0,
        "roomy observer was not re-delivered on the retry");

   detach(&pub);
   detach(&full_obs);
   detach(&roomy_obs);
   bus_host_destroy(&h);
   printf("  block fan-out: the served observer is not re-delivered on retry\n");
}

static void test_shed_emits_overflow(void)
{
   int worm_before = worm_count("bus.overflow");
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");
   g_n = 0;
   g_loss_n = 0;
   bus_host_set_tap(&h, tap, NULL);
   bus_host_set_loss_sink(&h, loss_sink, NULL);

   client_t pub, obs;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs);
   must(bus_host_subscribe(&h, obs.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");
   must(bus_host_set_kind_policy(&h, KIND_A, BUS_KIND_SHED) == BUS_HOST_OK, "KIND_A sheds");

   uint32_t lim = data_limit(&obs);
   /* Fill the data allowance, then emit one more: it must be shed with a typed
    * overflow rather than blocking or vanishing. */
   for (uint32_t i = 0; i < lim; i++)
      emit(&pub, BUS_F_NOTIFICATION, KIND_A, 4, 0);
   emit(&pub, BUS_F_NOTIFICATION, KIND_A, 4, 0xEE); /* the one that overflows */
   bus_host_pump(&h);
   must(!h.slots[pub.reply.handle_id].blocked, "shed does not block the producer");

   /* Drain the data events, then the overflow (control-class, from the reserve)
    * naming the shed seq. */
   bus_frame_t f;
   uint8_t buf[32];
   uint32_t data = 0, overflow = 0;
   while (recv_event(&obs, &f, buf, sizeof buf))
   {
      if (f.event_kind == BUS_KIND_OVERFLOW)
      {
         overflow++;
         must(f.hdr_flags & BUS_F_CONTROL, "overflow is control-class");
         bus_overflow_t ov;
         memcpy(&ov, buf, sizeof ov);
         must(ov.shed_kind == KIND_A, "overflow names the shed kind");
         must(ov.dst_slot == obs.reply.handle_id, "overflow names the destination");
         /* The shed seq was a real tapped seq. */
         int found = 0;
         for (uint32_t i = 0; i < g_n; i++)
            if (g_seq[i] == ov.shed_seq && g_kind[i] == KIND_A)
               found = 1;
         must(found, "the shed seq was a genuine tapped event");
      }
      else
      {
         data++;
      }
   }
   must(data == lim, "the data allowance was delivered");
   must(overflow == 1, "exactly one overflow for the one shed event");
   must(g_loss_n == 1 && g_loss_frame.event_kind == BUS_KIND_OVERFLOW,
        "overflow escaped the capture-only tap through the loss sink");
   must(g_loss_payload_len == sizeof(bus_overflow_t), "loss sink received typed overflow");
   bus_overflow_t durable_overflow;
   memcpy(&durable_overflow, g_loss_payload, sizeof durable_overflow);
   must(durable_overflow.shed_kind == KIND_A && durable_overflow.dst_slot == obs.reply.handle_id,
        "durable overflow names the lost kind and destination");
   must(worm_count("bus.overflow") == worm_before + 1, "overflow reached the durable WORM ledger");

   detach(&pub);
   detach(&obs);
   bus_host_destroy(&h);
   printf("  shed: a full destination gets a typed overflow naming the lost seq\n");
}

static void test_control_lost_when_reserve_exhausted(void)
{
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");

   client_t pub, obs;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs);
   must(bus_host_subscribe(&h, obs.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");
   must(bus_host_set_kind_policy(&h, KIND_A, BUS_KIND_SHED) == BUS_HOST_OK, "shed");

   /* Fill the ENTIRE inbound ring (data allowance + the whole control reserve)
    * by shedding far more than capacity, so even overflow events cannot fit.
    * The consumer never drains, so the reserve fills and control_lost latches. */
   uint32_t cap = obs.reply.queue_capacity;
   for (uint32_t i = 0; i < cap * 3; i++)
   {
      /* Re-emit as the outbound ring drains via pump each time. */
      emit(&pub, BUS_F_NOTIFICATION, KIND_A, 4, 0);
      bus_host_pump(&h);
   }
   must(atomic_load_explicit(&obs.qp.hdr->control_lost, memory_order_acquire) == 1,
        "control_lost latched when even the reserve was exhausted");

   detach(&pub);
   detach(&obs);
   bus_host_destroy(&h);
   printf("  control_lost: latches when the reserve itself is exhausted\n");
}

static void test_producer_reaped_tap_only(void)
{
   int worm_before = worm_count("bus.producer_reaped");
   bus_host_config_t c = cfg();
   bus_host_t h;
   must(bus_host_create(&h, &c, NULL, NULL) == BUS_HOST_OK, "host");
   g_n = 0;
   g_loss_n = 0;
   bus_host_set_tap(&h, tap, NULL);
   bus_host_set_loss_sink(&h, loss_sink, NULL);

   client_t pub, obs;
   attach(&h, 1, &pub);
   attach(&h, 2, &obs);
   must(bus_host_subscribe(&h, obs.reply.handle_id, KIND_A) == BUS_HOST_OK, "subscribe");

   /* Block the producer on the full (never-draining) destination. */
   uint32_t lim = data_limit(&obs);
   for (uint32_t i = 0; i <= lim; i++)
      emit(&pub, BUS_F_NOTIFICATION, KIND_A, 4, 0);
   bus_host_pump(&h);
   must(h.slots[pub.reply.handle_id].blocked, "producer blocked");

   uint32_t before = tap_count_kind(BUS_KIND_PRODUCER_REAPED);
   /* Reap the blocked producer: its held event is discarded and named to the
    * tap as producer_reaped — recorded, delivered to no one. */
   atomic_store_explicit(&pub.qp.hdr->client_heartbeat, 0, memory_order_release);
   bus_host_reap(&h, 100, 10); /* first tick starts the clock */
   bus_host_reap(&h, 200, 10); /* now stale -> reaped */
   must(!h.slots[pub.reply.handle_id].in_use, "producer reaped");
   must(tap_count_kind(BUS_KIND_PRODUCER_REAPED) == before + 1,
        "the discarded in-flight event was named to the tap as producer_reaped");
   must(g_loss_n == 1 && g_loss_frame.event_kind == BUS_KIND_PRODUCER_REAPED,
        "producer reap escaped the capture-only tap through the loss sink");
   must(g_loss_payload_len == sizeof(bus_producer_reaped_t),
        "loss sink received typed producer reap");
   bus_producer_reaped_t durable_reap;
   memcpy(&durable_reap, g_loss_payload, sizeof durable_reap);
   must(durable_reap.lost_seq != 0 && durable_reap.lost_kind == KIND_A &&
            durable_reap.src_slot == pub.reply.handle_id,
        "durable reap names the lost sequence, kind, and producer");
   must(worm_count("bus.producer_reaped") == worm_before + 1,
        "producer reap reached the durable WORM ledger");

   detach(&pub);
   detach(&obs);
   bus_host_destroy(&h);
   printf("  producer_reaped: a discarded in-flight event is named to the tap\n");
}

int main(void)
{
   char worm_path[512];
   snprintf(worm_path, sizeof worm_path, "%s/aimee-bus-loss-%d.db", platform_tmpdir(),
            (int)getpid());
   unlink(worm_path);
   must(audit_worm_init_at(worm_path) == 0, "initialize WORM loss ledger");
   must(obs_bus_set_durable_sink(worm_sink, NULL) == 0, "install WORM loss sink");
   printf("test_bus_flow:\n");
   test_block_holds_then_delivers();
   test_block_is_per_producer();
   test_block_fanout_no_double_deliver();
   test_shed_emits_overflow();
   test_control_lost_when_reserve_exhausted();
   test_producer_reaped_tap_only();
   must(audit_worm_verify_chain(NULL, 0) == 0, "loss ledger hash chain verifies");
   audit_worm_close();
   unlink(worm_path);
   printf("test_bus_flow: OK\n");
   return 0;
}
