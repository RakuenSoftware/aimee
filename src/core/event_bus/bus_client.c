/* bus_client.c: the C reference bus client. See bus_client.h. */
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_ring.h>

bus_client_result_t bus_client_attach(int sock, bus_client_t *c)
{
   return bus_client_attach_as(sock, c, 0, 0);
}

bus_client_result_t bus_client_attach_as(int sock, bus_client_t *c, uint32_t principal_class,
                                         uint32_t principal_ref)
{
   if (!c || sock < 0)
      return BUS_CLIENT_ERR_ARG;
   memset(c, 0, sizeof *c);

   bus_attach_request_t req;
   memset(&req, 0, sizeof req);
   req.magic = BUS_ATTACH_REQ_MAGIC;
   req.wire_version_min = BUS_WIRE_VERSION;
   req.wire_version_max = BUS_WIRE_VERSION;
   req.principal_class = principal_class;
   req.principal_ref = principal_ref;
   if (bus_fd_send(sock, &req, sizeof req, NULL, 0) != 0)
      return BUS_CLIENT_ERR_OS;

   int fds[BUS_ATTACH_FDS];
   int nfd = 0;
   long n = bus_fd_recv(sock, &c->reply, sizeof c->reply, fds, BUS_ATTACH_FDS, &nfd);
   if (n != (long)sizeof c->reply || c->reply.magic != BUS_ATTACH_REPLY_MAGIC)
   {
      for (int i = 0; i < nfd; i++)
         close(fds[i]);
      return BUS_CLIENT_ERR_PROTOCOL;
   }

   c->attach_status = (bus_attach_status_t)c->reply.status;
   if (c->reply.status != BUS_ATTACH_OK)
   {
      for (int i = 0; i < nfd; i++)
         close(fds[i]);
      return BUS_CLIENT_DENIED;
   }
   if (nfd != BUS_ATTACH_FDS)
   {
      for (int i = 0; i < nfd; i++)
         close(fds[i]);
      return BUS_CLIENT_ERR_PROTOCOL;
   }

   /* control (read-only), arena (rw), own queue pair (rw). */
   bus_client_result_t rc = BUS_CLIENT_ERR_OS;
   /* A D12 host's control region carries the lease view after the header; an
    * older host's is the header alone. Map what is there. */
   struct stat cst;
   if (fstat(fds[BUS_ATTACH_FD_CONTROL], &cst) != 0)
      goto done;
   size_t ctl_bytes = (cst.st_size >= 0 && (size_t)cst.st_size >= bus_control_bytes())
                          ? bus_control_bytes()
                          : bus_control_header_bytes();
   if (bus_region_map(fds[BUS_ATTACH_FD_CONTROL], ctl_bytes, 0, &c->control) != BUS_REGION_OK)
      goto done;
   if (bus_region_map(fds[BUS_ATTACH_FD_ARENA], bus_arena_region_bytes(c->reply.arena_size), 1,
                      &c->arena) != BUS_REGION_OK)
      goto done;
   if (bus_region_map(fds[BUS_ATTACH_FD_QPAIR],
                      bus_qpair_bytes(c->reply.slot_size, c->reply.queue_capacity), 1,
                      &c->qpair) != BUS_REGION_OK)
      goto done;
   if (bus_control_attach(&c->control, &c->ctl) != BUS_REGION_OK)
      goto done;
   if (bus_qpair_attach(&c->qpair, &c->qp) != BUS_REGION_OK)
      goto done;
   if (bus_arena_region_attach(&c->arena, &c->arena_base, &c->arena_size) != BUS_REGION_OK)
      goto done;
   if (bus_control_lease_view(&c->control, c->ctl, &c->lease_view) != BUS_REGION_OK)
      c->lease_view = NULL; /* an older host: arena payloads stay unreadable */

   c->attached_epoch = c->reply.host_epoch;
   rc = BUS_CLIENT_OK;

done:
   /* The mappings hold their own references; the descriptors can be closed. */
   for (int i = 0; i < BUS_ATTACH_FDS; i++)
      close(fds[i]);
   if (rc != BUS_CLIENT_OK)
      bus_client_detach(c);
   return rc;
}

void bus_client_detach(bus_client_t *c)
{
   if (!c)
      return;
   bus_region_unmap(&c->control);
   bus_region_unmap(&c->arena);
   bus_region_unmap(&c->qpair);
   memset(c, 0, sizeof *c);
}

/* Encode a frame (and inline payload) into the outbound ring. */
static bus_client_result_t emit(bus_client_t *c, uint16_t flags, uint32_t kind, uint64_t corr,
                                const void *payload, uint32_t len)
{
   if (!c || c->ctl == NULL)
      return BUS_CLIENT_ERR_ARG;
   /* This is the inline path (a large payload goes via bus_client_publish_arena).
    * An inline payload sits after the 64-byte header, so it must fit the inline
    * budget AND leave the header room in the slot — a config with inline_budget ==
    * slot_size would otherwise let this overrun the ring slot. */
   if (len > 0 &&
       (len > c->reply.inline_budget || (uint64_t)BUS_WIRE_HDR_LEN + len > c->reply.slot_size))
      return BUS_CLIENT_ERR_PAYLOAD;
   if (len > 0 && !payload)
      return BUS_CLIENT_ERR_ARG;

   /* Fail fast if the host has gone; a stale mapping must not be written as if
    * it were live. */
   if (bus_client_epoch_changed(c))
      return BUS_CLIENT_EPOCH;

   uint8_t *slot = bus_ring_produce_begin(&c->qp.outbound);
   if (!slot)
      return BUS_CLIENT_WOULD_BLOCK;

   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.hdr_flags = flags | (len ? BUS_F_INLINE : 0);
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = kind;
   f.correlation_id = corr;
   f.src_handle = c->reply.handle_id;
   if (len)
   {
      f.payload_len = len;
      f.payload_ref = BUS_WIRE_HDR_LEN;
   }
   if (bus_wire_encode(&f, slot, c->reply.slot_size) != BUS_WIRE_HDR_LEN)
      return BUS_CLIENT_ERR_ARG;
   if (len)
      memcpy(slot + BUS_WIRE_HDR_LEN, payload, len);
   bus_ring_produce_commit(&c->qp.outbound);
   return BUS_CLIENT_OK;
}

bus_client_result_t bus_client_publish(bus_client_t *c, uint32_t kind, const void *payload,
                                       uint32_t len)
{
   return emit(c, BUS_F_NOTIFICATION, kind, 0, payload, len);
}

bus_client_result_t bus_client_publish_arena(bus_client_t *c, uint32_t kind, uint32_t lease_id,
                                             uint32_t generation, uint32_t len)
{
   if (!c || c->ctl == NULL)
      return BUS_CLIENT_ERR_ARG;
   /* A zero-length arena payload is a contradiction — there is nothing to lease.
    * len is bounded by the arena, not the inline budget; the lease table already
    * refused an over-large allocation, so no size check is duplicated here. */
   if (len == 0)
      return BUS_CLIENT_ERR_PAYLOAD;
   if (bus_client_epoch_changed(c))
      return BUS_CLIENT_EPOCH;

   uint8_t *slot = bus_ring_produce_begin(&c->qp.outbound);
   if (!slot)
      return BUS_CLIENT_WOULD_BLOCK;

   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.hdr_flags = BUS_F_NOTIFICATION | BUS_F_ARENA;
   f.wire_version = BUS_WIRE_VERSION;
   f.event_kind = kind;
   f.src_handle = c->reply.handle_id;
   f.payload_ref = lease_id; /* ARENA (v2): payload_ref carries the lease id */
   f.generation = generation;
   f.payload_len = len;
   if (bus_wire_encode(&f, slot, c->reply.slot_size) != BUS_WIRE_HDR_LEN)
      return BUS_CLIENT_ERR_ARG;
   bus_ring_produce_commit(&c->qp.outbound);
   return BUS_CLIENT_OK;
}

bus_client_result_t bus_client_request(bus_client_t *c, uint32_t kind, uint64_t correlation,
                                       const void *payload, uint32_t len)
{
   return bus_client_request_fragment(c, kind, correlation, payload, len, 0);
}

bus_client_result_t bus_client_request_fragment(bus_client_t *c, uint32_t kind,
                                                uint64_t correlation, const void *payload,
                                                uint32_t len, int more)
{
   if (correlation == 0)
      return BUS_CLIENT_ERR_ARG; /* a correlated pattern needs a nonzero id */
   if (more && len == 0)
      return BUS_CLIENT_ERR_ARG;
   return emit(c, (uint16_t)(BUS_F_REQUEST | (more ? BUS_F_MORE : 0)), kind, correlation, payload,
               len);
}

bus_client_result_t bus_client_reply(bus_client_t *c, uint32_t kind, uint64_t correlation,
                                     const void *payload, uint32_t len)
{
   return bus_client_reply_fragment(c, kind, correlation, payload, len, 0);
}

bus_client_result_t bus_client_reply_fragment(bus_client_t *c, uint32_t kind, uint64_t correlation,
                                              const void *payload, uint32_t len, int more)
{
   if (correlation == 0)
      return BUS_CLIENT_ERR_ARG;
   if (more && len == 0)
      return BUS_CLIENT_ERR_ARG;
   return emit(c, (uint16_t)(BUS_F_REPLY | (more ? BUS_F_MORE : 0)), kind, correlation, payload,
               len);
}

bus_client_result_t bus_client_cancel(bus_client_t *c, uint32_t kind, uint64_t correlation)
{
   if (correlation == 0)
      return BUS_CLIENT_ERR_ARG;
   return emit(c, BUS_F_CANCEL, kind, correlation, NULL, 0);
}

int bus_client_arena_readable(const bus_client_t *c)
{
   return c && c->lease_view != NULL;
}

static bus_client_result_t send_release(bus_client_t *c, bus_client_lease_t l)
{
   uint8_t p[BUS_LEASE_RELEASE_LEN];
   for (unsigned i = 0; i < 4; i++)
   {
      p[BUS_LEASE_RELEASE_OFF_LEASE + i] = (uint8_t)(l.lease_id >> (8u * i));
      p[BUS_LEASE_RELEASE_OFF_GENERATION + i] = (uint8_t)(l.generation >> (8u * i));
   }
   return emit(c, BUS_F_NOTIFICATION | BUS_F_CONTROL, BUS_KIND_LEASE_RELEASE, 0, p, sizeof p);
}

bus_client_result_t bus_client_flush_releases(bus_client_t *c)
{
   if (!c || c->ctl == NULL)
      return BUS_CLIENT_ERR_ARG;
   uint32_t sent = 0;
   while (sent < c->release_count)
   {
      bus_client_result_t r = send_release(c, c->releases[sent]);
      if (r == BUS_CLIENT_WOULD_BLOCK)
         break;
      if (r == BUS_CLIENT_EPOCH)
         return r; /* the host restarted: every ref died with it */
      sent++;      /* sent, or unsendable: either way not retried */
   }
   memmove(c->releases, c->releases + sent, (c->release_count - sent) * sizeof c->releases[0]);
   c->release_count -= sent;
   return c->release_count ? BUS_CLIENT_WOULD_BLOCK : BUS_CLIENT_OK;
}

bus_client_result_t bus_client_poll(bus_client_t *c, bus_event_t *out)
{
   if (!c || !out || c->ctl == NULL)
      return BUS_CLIENT_ERR_ARG;

   /* After a host restart every mapping is stale — the inbound ring belongs to a
    * defunct region and will never carry a new event. Fail closed rather than
    * hand back a stale frame; the caller must re-attach. */
   if (bus_client_epoch_changed(c))
      return BUS_CLIENT_EPOCH;

   /* Release the slot handed out by the previous poll — its payload pointer is
    * now invalid, which is the documented contract. */
   if (c->have_pending_read)
   {
      bus_ring_consume_commit(&c->qp.inbound);
      c->have_pending_read = 0;
   }
   /* The arena span handed out by the previous poll is no longer the caller's
    * to read: return its ref (D12). */
   if (c->have_pending_lease)
   {
      c->releases[c->release_count++] = c->pending_lease;
      c->have_pending_lease = 0;
   }
   if (c->release_count && bus_client_flush_releases(c) == BUS_CLIENT_EPOCH)
      return BUS_CLIENT_EPOCH;

   const uint8_t *slot = bus_ring_consume_begin(&c->qp.inbound);
   if (!slot)
      return BUS_CLIENT_EMPTY;

   memset(out, 0, sizeof *out);
   if (bus_wire_decode_checked(slot, c->reply.slot_size, c->reply.slot_size, c->arena_size,
                               &out->frame) != BUS_WIRE_OK)
   {
      /* A malformed inbound frame should never happen — the host encodes it —
       * but if it does, drop it rather than hand back garbage. */
      bus_ring_consume_commit(&c->qp.inbound);
      return BUS_CLIENT_EMPTY;
   }
   if ((out->frame.hdr_flags & BUS_F_INLINE) && out->frame.payload_len > 0)
   {
      out->payload = slot + out->frame.payload_ref;
      out->payload_len = out->frame.payload_len;
   }
   else if ((out->frame.hdr_flags & BUS_F_ARENA) && c->lease_view)
   {
      /* Delivering an arena frame gave this client a consumer ref, which it owes
       * back whether or not the payload resolves. With the release backlog
       * full, leave the frame unread until the outbound ring drains. */
      if (c->release_count >= BUS_CLIENT_MAX_PENDING_RELEASES)
         return BUS_CLIENT_EMPTY;
      uint64_t off = 0;
      if (bus_lease_view_resolve(c->lease_view, (uint32_t)out->frame.payload_ref,
                                 out->frame.generation, out->frame.payload_len, c->arena_size,
                                 &off) == BUS_REGION_OK)
      {
         out->payload = c->arena_base + off;
         out->payload_len = out->frame.payload_len;
      }
      c->pending_lease.lease_id = (uint32_t)out->frame.payload_ref;
      c->pending_lease.generation = out->frame.generation;
      c->have_pending_lease = 1;
   }
   c->have_pending_read = 1;
   return BUS_CLIENT_OK;
}

void bus_client_heartbeat(bus_client_t *c, uint64_t now)
{
   if (c && c->qp.hdr)
      atomic_store_explicit(&c->qp.hdr->client_heartbeat, now, memory_order_release);
}

int bus_client_epoch_changed(const bus_client_t *c)
{
   return c && c->ctl && bus_control_epoch_changed(c->ctl, c->attached_epoch);
}

int bus_client_control_lost(const bus_client_t *c)
{
   return c && c->qp.hdr &&
          atomic_load_explicit(&c->qp.hdr->control_lost, memory_order_acquire) != 0;
}

const char *bus_client_result_name(bus_client_result_t r)
{
   switch (r)
   {
   case BUS_CLIENT_OK:
      return "OK";
   case BUS_CLIENT_WOULD_BLOCK:
      return "WOULD_BLOCK";
   case BUS_CLIENT_EMPTY:
      return "EMPTY";
   case BUS_CLIENT_DENIED:
      return "DENIED";
   case BUS_CLIENT_EPOCH:
      return "EPOCH";
   case BUS_CLIENT_ERR_ARG:
      return "ERR_ARG";
   case BUS_CLIENT_ERR_PAYLOAD:
      return "ERR_PAYLOAD";
   case BUS_CLIENT_ERR_OS:
      return "ERR_OS";
   case BUS_CLIENT_ERR_PROTOCOL:
      return "ERR_PROTOCOL";
   default:
      return "ERR_UNKNOWN";
   }
}
