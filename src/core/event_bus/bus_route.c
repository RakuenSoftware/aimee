/* bus_route.c: the host's routing, governance tap, and flow control (D5/D6).
 *
 * The host drains each admitted client's outbound ring. For every event it
 * stamps a monotonic seq, offers it to the tap (the single full-stream observer,
 * before any routing decision — D6), then delivers by pattern under a
 * credit-based flow-control discipline:
 *
 *   - The drain loop never blocks on a destination. A full destination is
 *     resolved by that kind's policy immediately, and the loop moves on.
 *   - BLOCK (the default): the event is left at the producer's ring head,
 *     uncommitted, and retried next pump. The host holds at most this one
 *     in-flight event per producer, so backpressure propagates to the producer
 *     (its outbound stops draining) without an unbounded host queue. It is
 *     seq-stamped and tapped once; retries deliver only to the observers that
 *     have not yet received it.
 *   - SHED (opt-in per kind): a full destination is sent a typed overflow event
 *     naming the lost seq/kind — never a silent drop.
 *
 * Control-class events (overflow, producer_reaped, capability_absent) draw on a
 * reserve carved from the tail of each inbound ring, so a data-saturated client
 * still has room to be told what it lost. If even the reserve is full, a sticky
 * control_lost flag is set — the degenerate-case backstop that keeps "never
 * silently" true.
 *
 * Arena payloads (D3) are routed by reference, not by copy. The bytes live in the
 * shared arena; the host only forwards the lease and manages its refcount. On an
 * arena NOTIFICATION the host snapshots the kind's observers and publishes the
 * lease to exactly them (bus_arena_publish: +1 consumer ref each, -1 producer
 * ref), then forwards the reference frame under the same BLOCK/SHED discipline as
 * an inline fan-out — a SHED-full observer that will never read has its ref
 * released so the lease still drains. The host never dereferences an arena
 * offset; only a (co-located, trusted) consumer does, gated by the lease table's
 * generation + holder checks. A correlated arena pattern (request/reply/cancel)
 * is not routed yet — its lease is reclaimed so it cannot leak — until the memory
 * round-trip that needs it lands.
 */
#include <string.h>

#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_ring.h>
#include <aimee/core/event_bus/bus_wire.h>

#define KIND_SERVER_NONE (-1)

/* ---- slot bitmaps ---- */

static void obs_set(uint64_t *bits, uint32_t slot)
{
   bits[slot / 64] |= (uint64_t)1 << (slot % 64);
}
static void obs_clear(uint64_t *bits, uint32_t slot)
{
   bits[slot / 64] &= ~((uint64_t)1 << (slot % 64));
}
static int obs_test(const uint64_t *bits, uint32_t slot)
{
   return (bits[slot / 64] >> (slot % 64)) & 1;
}

/* ---- registry ---- */

static bus_kind_t *kind_find(bus_host_t *h, uint32_t kind)
{
   for (uint32_t i = 0; i < BUS_HOST_MAX_KINDS; i++)
      if (h->kinds[i].in_use && h->kinds[i].kind == kind)
         return &h->kinds[i];
   return NULL;
}

static bus_kind_t *kind_intern(bus_host_t *h, uint32_t kind)
{
   bus_kind_t *k = kind_find(h, kind);
   if (k)
      return k;
   for (uint32_t i = 0; i < BUS_HOST_MAX_KINDS; i++)
   {
      if (!h->kinds[i].in_use)
      {
         memset(&h->kinds[i], 0, sizeof h->kinds[i]);
         h->kinds[i].in_use = 1;
         h->kinds[i].kind = kind;
         h->kinds[i].server = KIND_SERVER_NONE;
         h->kinds[i].policy = BUS_KIND_BLOCK;
         return &h->kinds[i];
      }
   }
   return NULL;
}

void bus_host_set_tap(bus_host_t *h, bus_tap_fn fn, void *ctx)
{
   if (!h)
      return;
   h->tap = fn;
   h->tap_ctx = ctx;
}

bus_host_result_t bus_host_subscribe(bus_host_t *h, uint32_t slot, uint32_t event_kind)
{
   if (!h || slot >= h->cfg.max_slots || !h->slots[slot].in_use)
      return BUS_HOST_ERR_ARG;
   bus_kind_t *k = kind_intern(h, event_kind);
   if (!k)
      return BUS_HOST_ERR_ARG;
   obs_set(k->observers, slot);
   return BUS_HOST_OK;
}

bus_host_result_t bus_host_serve_kind(bus_host_t *h, uint32_t slot, uint32_t event_kind)
{
   if (!h || slot >= h->cfg.max_slots || !h->slots[slot].in_use)
      return BUS_HOST_ERR_ARG;
   bus_kind_t *k = kind_intern(h, event_kind);
   if (!k)
      return BUS_HOST_ERR_ARG;
   if (k->server != KIND_SERVER_NONE && k->server != (int32_t)slot)
      return BUS_HOST_ERR_ARG;
   k->server = (int32_t)slot;
   return BUS_HOST_OK;
}

int bus_host_kind_has_server(const bus_host_t *h, uint32_t event_kind)
{
   if (!h || !h->slots)
      return 0;
   for (uint32_t i = 0; i < BUS_HOST_MAX_KINDS; ++i)
   {
      const bus_kind_t *kind = &h->kinds[i];
      if (!kind->in_use || kind->kind != event_kind || kind->server == KIND_SERVER_NONE)
         continue;
      return (uint32_t)kind->server < h->cfg.max_slots && h->slots[kind->server].in_use;
   }
   return 0;
}

bus_host_result_t bus_host_enforce_grants(bus_host_t *h, uint32_t slot)
{
   if (!h || slot >= h->cfg.max_slots || !h->slots[slot].in_use)
      return BUS_HOST_ERR_ARG;
   h->slots[slot].enforce_grants = 1;
   return BUS_HOST_OK;
}

bus_host_result_t bus_host_grant_outbound(bus_host_t *h, uint32_t slot, uint32_t event_kind,
                                          uint8_t patterns)
{
   if (!h || slot >= h->cfg.max_slots || !h->slots[slot].in_use ||
       (patterns & ~(BUS_GRANT_NOTIFY | BUS_GRANT_REQUEST)) != 0 || patterns == 0 ||
       event_kind < BUS_KIND_MODULE_BASE)
      return BUS_HOST_ERR_ARG;
   bus_slot_t *s = &h->slots[slot];
   for (uint32_t i = 0; i < s->grant_count; i++)
      if (s->grants[i].kind == event_kind)
      {
         s->grants[i].patterns |= patterns;
         return BUS_HOST_OK;
      }
   if (s->grant_count >= BUS_HOST_MAX_GRANTS)
      return BUS_HOST_ERR_ARG;
   s->grants[s->grant_count].kind = event_kind;
   s->grants[s->grant_count].patterns = patterns;
   s->grant_count++;
   return BUS_HOST_OK;
}

bus_host_result_t bus_host_set_kind_policy(bus_host_t *h, uint32_t event_kind,
                                           bus_kind_policy_t policy)
{
   if (!h || (policy != BUS_KIND_BLOCK && policy != BUS_KIND_SHED))
      return BUS_HOST_ERR_ARG;
   bus_kind_t *k = kind_intern(h, event_kind);
   if (!k)
      return BUS_HOST_ERR_ARG;
   k->policy = policy;
   return BUS_HOST_OK;
}

/* ---- delivery primitives ---- */

/* Free inbound slots available to a class of traffic. Data may use all but the
 * control reserve; control may use everything. bus_ring_count is the host's own
 * view of a ring only it produces into, so it never over-fills; a stale count
 * (the client just freed a slot) only defers a data event, which is safe. */
static int has_room(const bus_slot_t *d, int is_control)
{
   uint32_t cap = bus_ring_capacity(&d->qpair.inbound);
   uint32_t reserve = atomic_load_explicit(&d->qpair.hdr->control_credits, memory_order_relaxed);
   uint64_t used = bus_ring_count(&d->qpair.inbound);
   uint32_t limit = is_control ? cap : (reserve < cap ? cap - reserve : 0);
   return used < limit;
}

/* Encode a (seq/dst-stamped) frame and any inline payload into a destination
 * inbound slot. Assumes has_room already said yes. Returns 1 on success. */
static int put(bus_host_t *h, bus_slot_t *d, const bus_frame_t *f, const uint8_t *inline_src)
{
   uint8_t *slot = bus_ring_produce_begin(&d->qpair.inbound);
   if (!slot)
      return 0;
   bus_frame_t out = *f;
   if (bus_wire_encode(&out, slot, h->cfg.slot_size) != BUS_WIRE_HDR_LEN)
      return 0;
   if ((out.hdr_flags & BUS_F_INLINE) && out.payload_len > 0)
   {
      if ((uint64_t)out.payload_ref + out.payload_len > h->cfg.slot_size)
         return 0;
      memcpy(slot + out.payload_ref, inline_src, out.payload_len);
   }
   bus_ring_produce_commit(&d->qpair.inbound);
   return 1;
}

/* Emit a host-generated control-class event (overflow, producer_reaped,
 * capability_absent): stamp seq, tap, deliver from the reserve. If even the
 * reserve is full, set the destination's sticky control_lost. dst < 0 means
 * tap-only (producer_reaped names a client that no longer exists). */
static void emit_control(bus_host_t *h, int dst, uint32_t kind, uint64_t corr, const void *payload,
                         uint32_t plen)
{
   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = kind;
   f.correlation_id = corr;
   f.hdr_flags = (corr ? BUS_F_REPLY : BUS_F_NOTIFICATION) | BUS_F_CONTROL;
   if (plen)
   {
      f.hdr_flags |= BUS_F_INLINE;
      f.payload_len = plen;
      f.payload_ref = BUS_WIRE_HDR_LEN;
   }
   f.seq = ++h->seq;
   f.dst_handle = dst < 0 ? 0 : (uint32_t)dst;
   if (h->tap)
      h->tap(h->tap_ctx, &f, (const uint8_t *)payload, plen);
   if (dst < 0)
      return; /* tap-only */
   bus_slot_t *d = &h->slots[dst];
   if (has_room(d, 1) && put(h, d, &f, payload))
      return;
   atomic_store_explicit(&d->qpair.hdr->control_lost, 1, memory_order_release);
}

/* Shed one data event to a full destination: tell it, via an overflow event,
 * exactly which seq/kind it lost. */
static void shed(bus_host_t *h, uint32_t dst, const bus_frame_t *shed_f)
{
   bus_overflow_t ov = {.shed_seq = shed_f->seq, .shed_kind = shed_f->event_kind, .dst_slot = dst};
   emit_control(h, (int)dst, BUS_KIND_OVERFLOW, 0, &ov, (uint32_t)sizeof ov);
}

/* ---- forget a departing slot ---- */

void bus_route_forget_slot(bus_host_t *h, uint32_t slot)
{
   /* If the slot had a block-held event in flight, it is discarded now: name its
    * seq to the tap as producer_reaped so the loss is recorded, not silent. */
   if (h->slots[slot].blocked)
      emit_control(h, -1, BUS_KIND_PRODUCER_REAPED, 0, NULL, 0);

   for (uint32_t i = 0; i < BUS_HOST_MAX_KINDS; i++)
   {
      if (!h->kinds[i].in_use)
         continue;
      obs_clear(h->kinds[i].observers, slot);
      if (h->kinds[i].server == (int32_t)slot)
         h->kinds[i].server = KIND_SERVER_NONE;
   }
   for (uint32_t i = 0; i < BUS_HOST_MAX_PENDING; i++)
      if (h->pending[i].in_use && (h->pending[i].requester == slot || h->pending[i].server == slot))
         h->pending[i].in_use = 0;
}

/* ---- pending request table ---- */

static bus_pending_t *pending_add(bus_host_t *h, uint64_t corr, uint32_t requester, uint32_t server,
                                  int request_open)
{
   for (uint32_t i = 0; i < BUS_HOST_MAX_PENDING; i++)
   {
      if (!h->pending[i].in_use)
      {
         h->pending[i].in_use = 1;
         h->pending[i].correlation_id = corr;
         h->pending[i].server_correlation_id = ++h->next_server_correlation;
         h->pending[i].requester = requester;
         h->pending[i].server = server;
         h->pending[i].request_open = request_open;
         return &h->pending[i];
      }
   }
   return NULL;
}

/* Look up by the requester's own numbering. Correlation ids only mean anything
 * alongside the client that chose them, so every caller-side lookup pairs the
 * two -- matching on the id alone is what let one client's request collide with
 * another's and be refused as a capability that was in fact being served. */
static bus_pending_t *pending_find_requester(bus_host_t *h, uint64_t corr, uint32_t requester)
{
   for (uint32_t i = 0; i < BUS_HOST_MAX_PENDING; i++)
      if (h->pending[i].in_use && h->pending[i].correlation_id == corr &&
          h->pending[i].requester == requester)
         return &h->pending[i];
   return NULL;
}

/* Look up by the id the host handed the server. That id is bus-unique, so the
 * server pairing only guards against a reply forged by a slot that is not the
 * one the request went to. */
static bus_pending_t *pending_find_server(bus_host_t *h, uint64_t corr, uint32_t server)
{
   for (uint32_t i = 0; i < BUS_HOST_MAX_PENDING; i++)
      if (h->pending[i].in_use && h->pending[i].server_correlation_id == corr &&
          h->pending[i].server == server)
         return &h->pending[i];
   return NULL;
}

/* Rewrite a frame's correlation for delivery without disturbing the original,
 * which the pump may re-route if a destination is full. */
static bus_frame_t frame_with_correlation(const bus_frame_t *f, uint64_t corr)
{
   bus_frame_t out = *f;
   out.correlation_id = corr;
   return out;
}

/* Register a new request or advance the only legal continuation: the same
 * requester/server pair after a fragment explicitly marked MORE. Correlation
 * reuse while a complete request is awaiting its reply is rejected. */
static bus_pending_t *pending_request(bus_host_t *h, const bus_frame_t *frame, uint32_t requester,
                                      uint32_t server)
{
   bus_pending_t *pending = pending_find_requester(h, frame->correlation_id, requester);
   if (pending)
   {
      if (pending->server != server || !pending->request_open)
         return NULL;
      pending->request_open = (frame->hdr_flags & BUS_F_MORE) != 0;
      return pending;
   }
   return pending_add(h, frame->correlation_id, requester, server,
                      (frame->hdr_flags & BUS_F_MORE) != 0);
}

/* ---- routing ---- */

static int slot_allows_fresh(const bus_slot_t *slot, const bus_frame_t *frame)
{
   if (!slot->enforce_grants)
      return 1;
   uint8_t wanted = 0;
   if (frame->hdr_flags & BUS_F_NOTIFICATION)
      wanted = BUS_GRANT_NOTIFY;
   else if (frame->hdr_flags & BUS_F_REQUEST)
      wanted = BUS_GRANT_REQUEST;
   else
      return 1; /* replies/cancels are bound to a pending request below */
   for (uint32_t i = 0; i < slot->grant_count; i++)
      if (slot->grants[i].kind == frame->event_kind && (slot->grants[i].patterns & wanted))
         return 1;
   return 0;
}

/* Route a notification to observers. Returns 1 if fully resolved (may commit the
 * source), 0 if a BLOCK-policy destination was full and the event must stay at
 * the producer's ring head. `delivered` tracks observers already served across
 * retries. */
static int route_notification(bus_host_t *h, uint32_t src, bus_frame_t *f, const uint8_t *inl,
                              bus_kind_t *k, uint64_t *delivered)
{
   int all_done = 1;
   for (uint32_t s = 0; s < h->cfg.max_slots; s++)
   {
      if (!h->slots[s].in_use || !obs_test(k->observers, s))
         continue;
      if (obs_test(delivered, s))
         continue; /* already delivered on a prior retry */
      bus_slot_t *d = &h->slots[s];
      if (has_room(d, 0) && put(h, d, f, inl))
      {
         obs_set(delivered, s);
         continue;
      }
      /* Full destination. */
      if (k->policy == BUS_KIND_SHED)
      {
         shed(h, s, f);
         obs_set(delivered, s); /* shed is resolution: never retried */
      }
      else
      {
         all_done = 0; /* BLOCK: leave the event in flight for this destination */
      }
   }
   return all_done;
}

/* Validate an arena frame against the authoritative lease before routing. The
 * host owns the lease table, so it can insist the frame's generation matches and
 * its payload_len does not exceed the span — which is what lets a consumer read
 * exactly payload_len bytes in place and trust they lie within the lease (a
 * producer-supplied length is not otherwise bounded). payload_len may be smaller
 * than the span (the allocator rounds up); it may not exceed it. A mismatch is a
 * producer bug or a stale/reused id, and the caller drops the frame — leaving the
 * lease for producer reap rather than reclaiming one that may now be another's. */
static int arena_frame_matches_lease(bus_host_t *h, const bus_frame_t *f)
{
   bus_arena_ref_t ref;
   if (bus_arena_ref(&h->arena, (uint32_t)f->payload_ref, &ref) != BUS_ARENA_OK)
      return 0;
   return ref.generation == f->generation && f->payload_len <= ref.len;
}

/* The single slot in a point-to-point target set, or UINT32_MAX if empty. */
static uint32_t arena_single_target(const uint64_t *targets, uint32_t max_slots)
{
   for (uint32_t s = 0; s < max_slots; s++)
      if (obs_test(targets, s))
         return s;
   return UINT32_MAX;
}

typedef enum
{
   ARENA_DEST_DELIVERED, /* the reference is in the destination's inbound ring */
   ARENA_DEST_BLOCKED,   /* full + BLOCK: retry next pump, the ref is still held */
   ARENA_DEST_SHED,      /* full + SHED: the ref was released, an overflow was sent */
   ARENA_DEST_GONE       /* the destination departed; its ref was dropped by reap */
} arena_dest_t;

/* Deliver an arena reference to one destination slot. The ref published to that
 * slot is released here on SHED (it will never be read); on GONE the reap already
 * dropped it; on DELIVERED the consumer releases it after reading. */
static arena_dest_t arena_deliver_one(bus_host_t *h, bus_frame_t *f, uint32_t lease,
                                      uint32_t generation, uint32_t s, bus_kind_policy_t policy)
{
   if (!h->slots[s].in_use)
      return ARENA_DEST_GONE;
   bus_slot_t *d = &h->slots[s];
   if (has_room(d, 0) && put(h, d, f, NULL))
      return ARENA_DEST_DELIVERED;
   if (policy == BUS_KIND_SHED)
   {
      bus_arena_release(&h->arena, lease, generation, s);
      shed(h, s, f);
      return ARENA_DEST_SHED;
   }
   return ARENA_DEST_BLOCKED;
}

/* Route an arena-payload notification by reference. At first sight it snapshots
 * the kind's observers into *targets and publishes the lease to exactly them, so
 * the refcount matches the set that will read; k==0 (no observers) reclaims the
 * lease immediately. Delivery is then retryable under BLOCK exactly like an
 * inline fan-out; a SHED-full observer's ref is released so the lease still
 * drains. The producer relinquishes the lease when it sends the frame; after
 * publish the host owns its lifetime (D3) and never touches the bytes. */
static int route_arena_notification(bus_host_t *h, uint32_t src, bus_frame_t *f, bus_kind_t *k,
                                    uint64_t *targets, uint64_t *delivered, int first_seen)
{
   uint32_t lease = (uint32_t)f->payload_ref;
   uint32_t generation = f->generation;

   if (first_seen)
   {
      if (!arena_frame_matches_lease(h, f))
      {
         h->slots[src].dropped++;
         return 1;
      }
      memset(targets, 0, BUS_ARENA_SLOT_WORDS * sizeof *targets);
      uint8_t obs[BUS_ARENA_MAX_SLOTS];
      uint32_t n = 0;
      if (k)
         for (uint32_t s = 0; s < h->cfg.max_slots; s++)
            if (h->slots[s].in_use && obs_test(k->observers, s))
            {
               obs_set(targets, s);
               obs[n++] = (uint8_t)s;
            }
      if (bus_arena_publish(&h->arena, lease, obs, n) != BUS_ARENA_OK)
      {
         h->slots[src].dropped++;
         return 1;
      }
      if (n == 0)
         return 1; /* published to nobody: the lease reclaimed itself */
   }

   int all_done = 1;
   for (uint32_t s = 0; s < h->cfg.max_slots; s++)
   {
      if (!obs_test(targets, s) || obs_test(delivered, s))
         continue;
      arena_dest_t r =
          arena_deliver_one(h, f, lease, generation, s, k ? k->policy : BUS_KIND_BLOCK);
      if (r == ARENA_DEST_BLOCKED)
         all_done = 0;
      else
         obs_set(delivered, s); /* DELIVERED, SHED, or GONE: this slot is resolved */
   }
   return all_done;
}

/* Route an arena-payload request point-to-point to the kind's server. Symmetric
 * to the notification path: validate, register the correlation, publish the lease
 * to the single server slot, then deliver the reference (retryable). No server ->
 * reclaim the lease and answer capability_absent. A shed request releases its ref
 * and drops the pending correlation, since no reply will ever come. */
static int route_arena_request(bus_host_t *h, uint32_t src, bus_frame_t *f, uint64_t *targets,
                               uint64_t *delivered, int first_seen)
{
   uint32_t lease = (uint32_t)f->payload_ref;
   uint32_t generation = f->generation;
   bus_kind_t *k = kind_find(h, f->event_kind);

   if (first_seen)
   {
      if (!arena_frame_matches_lease(h, f))
      {
         h->slots[src].dropped++;
         return 1;
      }
      if (!k || k->server == KIND_SERVER_NONE || !h->slots[k->server].in_use)
      {
         bus_arena_publish(&h->arena, lease, NULL, 0); /* reclaim: nobody to serve */
         emit_control(h, (int)src, BUS_KIND_CAPABILITY_ABSENT, f->correlation_id, NULL, 0);
         return 1;
      }
      if (!pending_request(h, f, src, (uint32_t)k->server))
      {
         bus_arena_publish(&h->arena, lease, NULL, 0); /* reclaim: no room to track it */
         emit_control(h, (int)src, BUS_KIND_CAPABILITY_ABSENT, f->correlation_id, NULL, 0);
         return 1;
      }
      uint8_t server = (uint8_t)k->server;
      memset(targets, 0, BUS_ARENA_SLOT_WORDS * sizeof *targets);
      obs_set(targets, server);
      if (bus_arena_publish(&h->arena, lease, &server, 1) != BUS_ARENA_OK)
      {
         bus_pending_t *p = pending_find_requester(h, f->correlation_id, src);
         if (p)
            p->in_use = 0;
         h->slots[src].dropped++;
         return 1;
      }
   }

   uint32_t s = arena_single_target(targets, h->cfg.max_slots);
   if (s == UINT32_MAX || obs_test(delivered, s))
      return 1;
   /* Resolved on every attempt, not just the first: a blocked delivery is
    * retried against the unmodified frame and must rewrite the same way. */
   bus_pending_t *out_pending = pending_find_requester(h, f->correlation_id, src);
   bus_frame_t out = frame_with_correlation(f, out_pending ? out_pending->server_correlation_id
                                                           : f->correlation_id);
   arena_dest_t r =
       arena_deliver_one(h, &out, lease, generation, s, k ? k->policy : BUS_KIND_BLOCK);
   if (r == ARENA_DEST_BLOCKED)
      return 0;
   if (r == ARENA_DEST_SHED)
   {
      /* The request was shed to a full server: no reply will come, so retire the
       * correlation rather than leave it dangling. */
      bus_pending_t *p = pending_find_requester(h, f->correlation_id, src);
      if (p)
         p->in_use = 0;
   }
   /* DELIVERED keeps the pending entry for the reply; GONE means the server was
    * reaped, which already retired the pending entry and dropped the ref. */
   obs_set(delivered, s);
   return 1;
}

/* Route an arena-payload reply back to the original requester. Only the kind's
 * server may answer a correlation; an unmatched or forged reply is reclaimed and
 * dropped. Delivered by reference; the correlation is retired once the reference
 * reaches the requester. A reply is never shed — a correlated answer blocks until
 * the requester has room, mirroring the inline reply path. */
static int route_arena_reply(bus_host_t *h, uint32_t src, bus_frame_t *f, uint64_t *targets,
                             uint64_t *delivered, int first_seen)
{
   uint32_t lease = (uint32_t)f->payload_ref;
   uint32_t generation = f->generation;

   if (first_seen)
   {
      if (!arena_frame_matches_lease(h, f))
      {
         h->slots[src].dropped++;
         return 1;
      }
      bus_pending_t *p = pending_find_server(h, f->correlation_id, src);
      if (!p || p->request_open || !h->slots[p->requester].in_use)
      {
         /* No matching request, a forged reply, or the requester departed: nothing
          * to deliver. Reclaim the lease; retire a real-but-undeliverable pending. */
         if (p)
            p->in_use = 0;
         else
            h->slots[src].dropped++; /* forged or unmatched: count the drop */
         bus_arena_publish(&h->arena, lease, NULL, 0);
         return 1;
      }
      uint8_t requester = (uint8_t)p->requester;
      memset(targets, 0, BUS_ARENA_SLOT_WORDS * sizeof *targets);
      obs_set(targets, requester);
      if (bus_arena_publish(&h->arena, lease, &requester, 1) != BUS_ARENA_OK)
      {
         h->slots[src].dropped++;
         return 1;
      }
   }

   uint32_t s = arena_single_target(targets, h->cfg.max_slots);
   if (s == UINT32_MAX || obs_test(delivered, s))
      return 1;
   bus_pending_t *out_pending = pending_find_server(h, f->correlation_id, src);
   bus_frame_t out =
       frame_with_correlation(f, out_pending ? out_pending->correlation_id : f->correlation_id);
   arena_dest_t r = arena_deliver_one(h, &out, lease, generation, s, BUS_KIND_BLOCK);
   if (r == ARENA_DEST_BLOCKED)
      return 0;
   /* DELIVERED or GONE: retire the correlation (a departed requester's ref was
    * already dropped by reap). */
   bus_pending_t *p = pending_find_server(h, f->correlation_id, src);
   if (p)
      p->in_use = 0;
   obs_set(delivered, s);
   return 1;
}

/* Route a fresh event (first time it is seen). Returns 1 if fully resolved. On a
 * BLOCK stall it returns 0 and sets *blocked_delivered so retries resume.
 * first_seen is 1 on the event's first pump, 0 on a block retry — an arena lease
 * is published exactly once, at first sight. */
static int route_fresh(bus_host_t *h, uint32_t src, bus_frame_t *f, const uint8_t *inl,
                       uint64_t *delivered, int first_seen)
{
   if (f->hdr_flags & BUS_F_ARENA)
   {
      uint64_t *targets = h->slots[src].arena_targets;
      if (f->hdr_flags & BUS_F_NOTIFICATION)
         return route_arena_notification(h, src, f, kind_find(h, f->event_kind), targets, delivered,
                                         first_seen);
      if (f->hdr_flags & BUS_F_REQUEST)
         return route_arena_request(h, src, f, targets, delivered, first_seen);
      if (f->hdr_flags & BUS_F_REPLY)
         return route_arena_reply(h, src, f, targets, delivered, first_seen);
      /* A cancel carries no meaningful payload; an arena cancel is malformed use.
       * Reclaim the lease so it cannot leak and drop it (the correlation is simply
       * not cancelled) rather than forward a reference to a reclaimed span. */
      bus_arena_publish(&h->arena, (uint32_t)f->payload_ref, NULL, 0);
      h->slots[src].dropped++;
      return 1;
   }

   bus_kind_t *k = kind_find(h, f->event_kind);

   if (f->hdr_flags & BUS_F_NOTIFICATION)
      return k ? route_notification(h, src, f, inl, k, delivered) : 1;

   if (f->hdr_flags & BUS_F_REQUEST)
   {
      if (!k || k->server == KIND_SERVER_NONE || !h->slots[k->server].in_use)
      {
         emit_control(h, (int)src, BUS_KIND_CAPABILITY_ABSENT, f->correlation_id, NULL, 0);
         return 1;
      }
      bus_slot_t *d = &h->slots[k->server];
      /* A request is point-to-point; block if the server is full. */
      if (!has_room(d, 0))
      {
         if (k->policy == BUS_KIND_SHED)
         {
            shed(h, (uint32_t)k->server, f);
            return 1;
         }
         return 0; /* block: retry next pump */
      }
      bus_pending_t *p = pending_request(h, f, src, (uint32_t)k->server);
      if (!p)
      {
         emit_control(h, (int)src, BUS_KIND_CAPABILITY_ABSENT, f->correlation_id, NULL, 0);
         return 1;
      }
      /* The server sees the host's bus-unique id, so it can key work on the
       * correlation alone without two callers ever colliding. */
      bus_frame_t out = frame_with_correlation(f, p->server_correlation_id);
      put(h, d, &out, inl);
      return 1;
   }

   if (f->hdr_flags & BUS_F_REPLY)
   {
      bus_pending_t *p = pending_find_server(h, f->correlation_id, src);
      if (!p || p->request_open)
         return 1; /* no matching request, or a forged reply; drop */
      uint32_t requester = p->requester;
      if (!h->slots[requester].in_use)
      {
         p->in_use = 0;
         return 1;
      }
      bus_slot_t *d = &h->slots[requester];
      if (!has_room(d, 0))
         return 0; /* block until the requester has room; keep the pending entry */
      if (!(f->hdr_flags & BUS_F_MORE))
         p->in_use = 0;
      /* Answer in the requester's own numbering, not the host's. */
      bus_frame_t out = frame_with_correlation(f, p->correlation_id);
      put(h, d, &out, inl);
      return 1;
   }

   if (f->hdr_flags & BUS_F_CANCEL)
   {
      bus_pending_t *p = pending_find_requester(h, f->correlation_id, src);
      if (p)
      {
         if (h->slots[p->server].in_use)
         {
            bus_slot_t *d = &h->slots[p->server];
            bus_frame_t out = frame_with_correlation(f, p->server_correlation_id);
            if (has_room(d, 0))
               put(h, d, &out, inl); /* best-effort */
         }
         /* The requester has abandoned the call and never waits for a terminal
          * reply. Retire the correlation now, including a partially assembled
          * fragmented request, so cancellation cannot exhaust the pending table. */
         p->in_use = 0;
      }
      return 1;
   }
   return 1;
}

uint32_t bus_host_pump(bus_host_t *h)
{
   if (!h || !h->slots)
      return 0;
   uint32_t routed = 0;

   for (uint32_t s = 0; s < h->cfg.max_slots; s++)
   {
      bus_slot_t *slot = &h->slots[s];
      if (!slot->in_use)
         continue;

      for (;;)
      {
         const uint8_t *ring_slot = bus_ring_consume_begin(&slot->qpair.outbound);
         if (!ring_slot)
            break;

         bus_frame_t f;
         if (bus_wire_decode(ring_slot, h->cfg.slot_size, &f) != BUS_WIRE_OK)
         {
            slot->dropped++;
            bus_ring_consume_commit(&slot->qpair.outbound);
            continue;
         }

         /* A client-supplied src/principal is never authority. Stamp both from
          * the admitted slot, then reject an undeclared fresh output before it
          * reaches the governance tap or any observer. */
         f.src_handle = s;
         f.principal_ref = slot->principal_ref;
         if (!slot_allows_fresh(slot, &f))
         {
            slot->dropped++;
            emit_control(h, (int)s, BUS_KIND_CAPABILITY_DENIED, f.correlation_id, NULL, 0);
            bus_ring_consume_commit(&slot->qpair.outbound);
            continue;
         }

         const uint8_t *inl = NULL;
         if ((f.hdr_flags & BUS_F_INLINE) && f.payload_len > 0 &&
             (uint64_t)f.payload_ref + f.payload_len <= h->cfg.slot_size)
            inl = ring_slot + f.payload_ref;

         int done;
         if (slot->blocked)
         {
            /* Same head event as last pump: reuse its seq, do not re-tap. */
            f.seq = slot->blocked_seq;
            done = route_fresh(h, s, &f, inl, slot->blocked_delivered, 0);
         }
         else
         {
            f.seq = ++h->seq;
            if (h->tap)
            {
               const uint8_t *tap_payload = inl;
               uint32_t tap_len = (f.hdr_flags & BUS_F_INLINE) ? f.payload_len : 0;
               /* Arena payloads are recorded too. Here, pre-routing, the producer
                * still holds the lease and has filled the span (its fill
                * happened-before the ring commit this pump observed), so the host
                * reads it ONCE to hand the bytes to the capture tap — the single
                * place the host reads arena bytes. Bounded by the span; validation
                * in route_arena_* then decides whether to route. */
               if (f.hdr_flags & BUS_F_ARENA)
               {
                  const uint8_t *ap = NULL;
                  uint32_t alen = 0;
                  if (bus_arena_producer_bytes(&h->arena, (uint32_t)f.payload_ref, &ap, &alen) ==
                      BUS_ARENA_OK)
                  {
                     tap_payload = ap;
                     tap_len = f.payload_len <= alen ? f.payload_len : alen;
                  }
               }
               h->tap(h->tap_ctx, &f, tap_payload, tap_len);
            }
            /* seq-stamped, pre-routing, once */
            memset(slot->blocked_delivered, 0, sizeof slot->blocked_delivered);
            done = route_fresh(h, s, &f, inl, slot->blocked_delivered, 1);
            routed++; /* count each accepted event once, at first sight */
         }

         if (done)
         {
            slot->blocked = 0;
            bus_ring_consume_commit(&slot->qpair.outbound);
         }
         else
         {
            /* BLOCK: leave the event at the ring head, stop draining this
             * producer this round. Its outbound stops moving, which is the
             * backpressure. */
            slot->blocked = 1;
            slot->blocked_seq = f.seq;
            break;
         }
      }
   }
   return routed;
}
