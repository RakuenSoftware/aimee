/* bus_route.c: the host's routing and governance tap. See bus_host.h.
 *
 * The host drains each admitted client's outbound ring, and for every event:
 * stamps a monotonic seq, offers it to the tap (the single full-stream observer,
 * before any routing decision — D6), then delivers it by pattern. All of this
 * runs in one thread, so seq order is the tap order and the capture order.
 *
 * This slice routes inline-payload events. An event whose payload lives in the
 * arena needs the host to publish that lease (slice 4) to the resolved observer
 * set before forwarding the reference; that composition is a named follow-up and
 * arena-flagged frames are dropped-with-count here rather than delivered to a
 * consumer that could not read them.
 */
#include <string.h>

#include "bus_host.h"
#include "bus_ring.h"
#include "bus_wire.h"

#define KIND_SERVER_NONE (-1)

/* ---- slot bitmap over observers ---- */

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
         h->kinds[i].in_use = 1;
         h->kinds[i].kind = kind;
         memset(h->kinds[i].observers, 0, sizeof h->kinds[i].observers);
         h->kinds[i].server = KIND_SERVER_NONE;
         return &h->kinds[i];
      }
   }
   return NULL; /* registry full */
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
      return BUS_HOST_ERR_ARG; /* one server per kind */
   k->server = (int32_t)slot;
   return BUS_HOST_OK;
}

/* When a slot departs (reap/detach), scrub it from every registry role and drop
 * any pending request it was party to. Called from slot_release in bus_host.c. */
void bus_route_forget_slot(bus_host_t *h, uint32_t slot)
{
   for (uint32_t i = 0; i < BUS_HOST_MAX_KINDS; i++)
   {
      if (!h->kinds[i].in_use)
         continue;
      obs_clear(h->kinds[i].observers, slot);
      if (h->kinds[i].server == (int32_t)slot)
         h->kinds[i].server = KIND_SERVER_NONE;
   }
   for (uint32_t i = 0; i < BUS_HOST_MAX_PENDING; i++)
      if (h->pending[i].in_use &&
          (h->pending[i].requester == slot || h->pending[i].server == slot))
         h->pending[i].in_use = 0;
}

/* ---- pending request table ---- */

static bus_pending_t *pending_add(bus_host_t *h, uint64_t corr, uint32_t requester,
                                  uint32_t server)
{
   for (uint32_t i = 0; i < BUS_HOST_MAX_PENDING; i++)
   {
      if (!h->pending[i].in_use)
      {
         h->pending[i].in_use = 1;
         h->pending[i].correlation_id = corr;
         h->pending[i].requester = requester;
         h->pending[i].server = server;
         return &h->pending[i];
      }
   }
   return NULL;
}

static bus_pending_t *pending_find(bus_host_t *h, uint64_t corr)
{
   for (uint32_t i = 0; i < BUS_HOST_MAX_PENDING; i++)
      if (h->pending[i].in_use && h->pending[i].correlation_id == corr)
         return &h->pending[i];
   return NULL;
}

/* ---- delivery ---- */

/* Write a (seq/dst-stamped) frame and its inline payload into a destination
 * slot's inbound ring. Returns 1 on delivery, 0 if the ring was full. The full
 * case is counted, not silent — slice 7 replaces the drop with credit-based
 * backpressure and a typed overflow event. */
static int deliver(bus_host_t *h, uint32_t dst_slot, const bus_frame_t *f,
                   const uint8_t *inline_src)
{
   bus_slot_t *d = &h->slots[dst_slot];
   uint8_t *slot = bus_ring_produce_begin(&d->qpair.inbound);
   if (!slot)
   {
      d->dropped++;
      return 0;
   }

   bus_frame_t out = *f;
   out.dst_handle = dst_slot;
   if (bus_wire_encode(&out, slot, h->cfg.slot_size) != BUS_WIRE_HDR_LEN)
   {
      d->dropped++;
      return 0;
   }
   if ((out.hdr_flags & BUS_F_INLINE) && out.payload_len > 0)
   {
      /* Inline payloads sit just past the header, inside the slot. */
      if ((uint64_t)out.payload_ref + out.payload_len > h->cfg.slot_size)
      {
         d->dropped++;
         return 0;
      }
      memcpy(slot + out.payload_ref, inline_src, out.payload_len);
   }
   bus_ring_produce_commit(&d->qpair.inbound);
   return 1;
}

/* Synthesize a capability_absent reply to a requester whose target kind has no
 * ready server. */
static void deliver_capability_absent(bus_host_t *h, uint32_t requester, uint64_t corr)
{
   bus_frame_t r;
   memset(&r, 0, sizeof r);
   r.hdr_flags = BUS_F_REPLY;
   r.wire_version = BUS_WIRE_VERSION;
   r.event_kind = BUS_KIND_CAPABILITY_ABSENT;
   r.correlation_id = corr;
   r.seq = ++h->seq;
   if (h->tap)
      h->tap(h->tap_ctx, &r);
   deliver(h, requester, &r, NULL);
}

/* Route one decoded event from `src_slot`. `inline_src` points at the inline
 * payload within the source ring slot (valid until the source is consumed). */
static void route_one(bus_host_t *h, uint32_t src_slot, bus_frame_t *f, const uint8_t *inline_src)
{
   f->seq = ++h->seq;
   f->src_handle = src_slot;

   /* The tap sees every seq-stamped event, before any routing decision. */
   if (h->tap)
      h->tap(h->tap_ctx, f);

   /* Arena-payload delivery is a separate integration; do not hand a consumer a
    * reference it cannot read. Counted against the source so it is not silent. */
   if (f->hdr_flags & BUS_F_ARENA)
   {
      h->slots[src_slot].dropped++;
      return;
   }

   bus_kind_t *k = kind_find(h, f->event_kind);

   if (f->hdr_flags & BUS_F_NOTIFICATION)
   {
      if (!k)
         return; /* nobody observes this kind */
      for (uint32_t s = 0; s < h->cfg.max_slots; s++)
         if (h->slots[s].in_use && obs_test(k->observers, s))
            deliver(h, s, f, inline_src);
      return;
   }

   if (f->hdr_flags & BUS_F_REQUEST)
   {
      if (!k || k->server == KIND_SERVER_NONE || !h->slots[k->server].in_use)
      {
         deliver_capability_absent(h, src_slot, f->correlation_id);
         return;
      }
      /* Point-to-point to the server, and remember the correlation so the reply
       * routes back to this requester only. */
      if (!pending_add(h, f->correlation_id, src_slot, (uint32_t)k->server))
      {
         deliver_capability_absent(h, src_slot, f->correlation_id);
         return;
      }
      deliver(h, (uint32_t)k->server, f, inline_src);
      return;
   }

   if (f->hdr_flags & BUS_F_REPLY)
   {
      bus_pending_t *p = pending_find(h, f->correlation_id);
      if (!p)
         return; /* no such outstanding request; drop */
      /* Only the server the request was routed to may answer it. Without this a
       * client could forge a reply for another client's correlation and have it
       * delivered to the requester. */
      if (p->server != src_slot)
         return;
      uint32_t requester = p->requester;
      p->in_use = 0;
      if (h->slots[requester].in_use)
         deliver(h, requester, f, inline_src);
      return;
   }

   if (f->hdr_flags & BUS_F_CANCEL)
   {
      bus_pending_t *p = pending_find(h, f->correlation_id);
      /* Only the original requester may cancel its own request. */
      if (p && p->requester == src_slot && h->slots[p->server].in_use)
         deliver(h, p->server, f, inline_src); /* best-effort */
      return;
   }
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
         if (bus_wire_decode(ring_slot, h->cfg.slot_size, &f) == BUS_WIRE_OK)
         {
            const uint8_t *inl = NULL;
            if ((f.hdr_flags & BUS_F_INLINE) && f.payload_len > 0 &&
                (uint64_t)f.payload_ref + f.payload_len <= h->cfg.slot_size)
               inl = ring_slot + f.payload_ref;
            route_one(h, s, &f, inl);
            routed++;
         }
         else
         {
            /* A malformed frame from a client is dropped, counted, and does not
             * stall the drain. */
            slot->dropped++;
         }
         bus_ring_consume_commit(&slot->qpair.outbound);
      }
   }
   return routed;
}
