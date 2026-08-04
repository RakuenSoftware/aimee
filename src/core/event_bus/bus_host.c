/* bus_host.c: bus host admission, directory, and reaping. See bus_host.h.
 *
 * The wire version this host speaks. A client declares a [min,max] range at
 * attach and the host picks the highest common value, or denies.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_ring.h>

/* Attach negotiation and frame validation must advertise the same version. */
#define BUS_HOST_WIRE_VERSION BUS_WIRE_VERSION

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */

/* Defined in bus_route.c: scrub a departing slot from the observer/server
 * registry and drop any pending request it was party to. */
void bus_route_forget_slot(bus_host_t *h, uint32_t slot);

static void slot_release(bus_host_t *h, uint32_t idx)
{
   bus_slot_t *s = &h->slots[idx];
   if (!s->in_use)
      return;
   bus_route_forget_slot(h, idx);
   /* A departing client's arena leases must not strand the arena: drop the refs
    * it held as a producer and as a consumer. */
   bus_arena_reap_producer(&h->arena, idx);
   bus_arena_reap_consumer(&h->arena, idx);
   bus_region_unmap(&s->qpair_region);
   if (s->qpair_fd >= 0)
      close(s->qpair_fd);
   memset(s, 0, sizeof *s);
   s->qpair_fd = -1;
   if (h->admitted > 0)
      h->admitted--;
}

bus_host_result_t bus_host_release_slot(bus_host_t *h, uint32_t slot)
{
   if (!h || !h->slots || slot >= h->cfg.max_slots || !h->slots[slot].in_use)
      return BUS_HOST_ERR_ARG;
   slot_release(h, slot);
   return BUS_HOST_OK;
}

bus_host_result_t bus_host_create(bus_host_t *h, const bus_host_config_t *cfg, bus_admit_fn admit,
                                  void *admit_ctx)
{
   if (!h || !cfg || cfg->max_slots == 0 || cfg->max_slots > BUS_ARENA_MAX_SLOTS)
      return BUS_HOST_ERR_ARG;

   memset(h, 0, sizeof *h);
   h->cfg = *cfg;
   h->admit = admit;
   h->admit_ctx = admit_ctx;
   h->control_fd = -1;
   h->arena_fd = -1;

   /* Control region. */
   if (bus_region_create("bus-control", bus_control_bytes(), &h->control_region) != BUS_REGION_OK)
      goto fail;
   h->control_fd = h->control_region.fd;
   if (bus_region_map(h->control_fd, h->control_region.size, 1, &h->control_region) !=
       BUS_REGION_OK)
      goto fail;
   if (bus_control_init(&h->control_region, cfg->slot_size, cfg->inline_budget, cfg->queue_capacity,
                        cfg->arena_size) != BUS_REGION_OK)
      goto fail;
   if (bus_control_attach(&h->control_region, &h->control) != BUS_REGION_OK)
      goto fail;

   /* Arena region + allocator. */
   size_t arena_bytes = bus_arena_region_bytes(cfg->arena_size);
   if (arena_bytes == 0)
      goto fail;
   if (bus_region_create("bus-arena", arena_bytes, &h->arena_region) != BUS_REGION_OK)
      goto fail;
   h->arena_fd = h->arena_region.fd;
   if (bus_region_map(h->arena_fd, h->arena_region.size, 1, &h->arena_region) != BUS_REGION_OK)
      goto fail;
   if (bus_arena_region_init(&h->arena_region, cfg->arena_size) != BUS_REGION_OK)
      goto fail;
   uint8_t *abase = NULL;
   uint64_t asize = 0;
   if (bus_arena_region_attach(&h->arena_region, &abase, &asize) != BUS_REGION_OK)
      goto fail;
   /* Per-client lease cap: capacity's worth per client is a sane provisional
    * bound, re-tuned in slice 12 with the other parameters. */
   if (bus_arena_init(&h->arena, abase, asize, cfg->max_slots, cfg->queue_capacity) != BUS_ARENA_OK)
      goto fail;

   h->slots = calloc(cfg->max_slots, sizeof(bus_slot_t));
   if (!h->slots)
      goto fail;
   for (uint32_t i = 0; i < cfg->max_slots; i++)
      h->slots[i].qpair_fd = -1;

   return BUS_HOST_OK;

fail:
   bus_host_destroy(h);
   return BUS_HOST_ERR_REGION;
}

void bus_host_set_admission(bus_host_t *h, bus_admit_fn admit, void *admit_ctx)
{
   if (!h)
      return;
   h->admit = admit;
   h->admit_ctx = admit ? admit_ctx : NULL;
}

void bus_host_destroy(bus_host_t *h)
{
   if (!h)
      return;
   if (h->slots)
   {
      for (uint32_t i = 0; i < h->cfg.max_slots; i++)
         slot_release(h, i);
      free(h->slots);
      h->slots = NULL;
   }
   bus_arena_fini(&h->arena);
   bus_region_unmap(&h->arena_region);
   if (h->arena_fd >= 0)
      close(h->arena_fd);
   bus_region_unmap(&h->control_region);
   if (h->control_fd >= 0)
      close(h->control_fd);
   h->arena_fd = h->control_fd = -1;
}

/* ------------------------------------------------------------------ */
/* admission                                                           */

static uint32_t find_free_slot(const bus_host_t *h)
{
   for (uint32_t i = 0; i < h->cfg.max_slots; i++)
      if (!h->slots[i].in_use)
         return i;
   return h->cfg.max_slots;
}

/* Send a denial reply with no descriptors. */
static bus_host_result_t deny(int conn_fd, bus_attach_status_t status)
{
   bus_attach_reply_t reply;
   memset(&reply, 0, sizeof reply);
   reply.magic = BUS_ATTACH_REPLY_MAGIC;
   reply.status = (uint32_t)status;
   bus_fd_send(conn_fd, &reply, sizeof reply, NULL, 0);
   return BUS_HOST_ERR_REFUSED;
}

bus_host_result_t bus_host_serve_attach(bus_host_t *h, int conn_fd)
{
   return bus_host_serve_attach_ex(h, conn_fd, NULL);
}

bus_host_result_t bus_host_serve_attach_ex(bus_host_t *h, int conn_fd, uint32_t *slot_out)
{
   if (slot_out)
      *slot_out = UINT32_MAX;
   if (!h || conn_fd < 0)
      return BUS_HOST_ERR_ARG;

   bus_attach_request_t req;
   int rfds[3];
   int nfd = 0;
   long n = bus_fd_recv(conn_fd, &req, sizeof req, rfds, 3, &nfd);
   /* A client never sends descriptors; if it tried, drop them. */
   for (int i = 0; i < nfd; i++)
      close(rfds[i]);
   if (n != (long)sizeof req || req.magic != BUS_ATTACH_REQ_MAGIC)
      return deny(conn_fd, BUS_ATTACH_PROTOCOL);

   /* Version negotiation: highest common, or deny. */
   if (req.wire_version_min > BUS_HOST_WIRE_VERSION || req.wire_version_max < BUS_HOST_WIRE_VERSION)
      return deny(conn_fd, BUS_ATTACH_DENIED_VERSION);

   /* Admission decision — identity/policy only. Default-admit when no seam is
    * injected (tests). */
   if (h->admit)
   {
      bus_attach_status_t d = h->admit(h->admit_ctx, conn_fd, &req);
      if (d != BUS_ATTACH_OK)
         return deny(conn_fd, d);
   }

   uint32_t idx = find_free_slot(h);
   if (idx == h->cfg.max_slots)
      return deny(conn_fd, BUS_ATTACH_DENIED_NOSLOT);

   /* Build this client's queue-pair region. Everything below can fail without
    * leaving a half-admitted slot: on any failure the slot is released. */
   bus_slot_t *s = &h->slots[idx];
   memset(s, 0, sizeof *s);
   s->qpair_fd = -1;

   size_t qbytes = bus_qpair_bytes(h->cfg.slot_size, h->cfg.queue_capacity);
   if (qbytes == 0)
      return deny(conn_fd, BUS_ATTACH_PROTOCOL);
   if (bus_region_create("bus-qpair", qbytes, &s->qpair_region) != BUS_REGION_OK)
   {
      slot_release(h, idx);
      return deny(conn_fd, BUS_ATTACH_DENIED_NOSLOT);
   }
   s->qpair_fd = s->qpair_region.fd;
   if (bus_region_map(s->qpair_fd, s->qpair_region.size, 1, &s->qpair_region) != BUS_REGION_OK ||
       bus_qpair_init(&s->qpair_region, h->cfg.slot_size, h->cfg.queue_capacity) != BUS_REGION_OK ||
       bus_qpair_attach(&s->qpair_region, &s->qpair) != BUS_REGION_OK)
   {
      slot_release(h, idx);
      return deny(conn_fd, BUS_ATTACH_DENIED_NOSLOT);
   }

   s->in_use = 1;
   s->principal_class = req.principal_class;
   s->principal_ref = req.principal_ref;
   s->last_heartbeat = 0;
   s->heartbeat_at = 0;
   h->admitted++;

   if (h->attach_hook)
   {
      bus_attach_status_t hook_status = h->attach_hook(h->attach_hook_ctx, h, idx, &req);
      if (hook_status != BUS_ATTACH_OK)
      {
         slot_release(h, idx);
         return deny(conn_fd, hook_status);
      }
   }

   /* Grant: the reply plus exactly three descriptors — control (the client will
    * map it read-only), arena, and this client's own queue pair. No other
    * client's descriptor is ever in this set. */
   bus_attach_reply_t reply;
   memset(&reply, 0, sizeof reply);
   reply.magic = BUS_ATTACH_REPLY_MAGIC;
   reply.status = BUS_ATTACH_OK;
   reply.handle_id = idx;
   reply.wire_version = BUS_HOST_WIRE_VERSION;
   reply.slot_size = h->cfg.slot_size;
   reply.inline_budget = h->cfg.inline_budget;
   reply.queue_capacity = h->cfg.queue_capacity;
   reply.arena_size = h->cfg.arena_size;
   reply.host_epoch = bus_control_epoch(h->control);

   /* "The control region is read-only to clients" (D1/D2) has to be enforced on
    * the DESCRIPTOR, not merely on the client's mapping: a client handed the
    * host's read-write control fd could map it PROT_WRITE and corrupt the region
    * for everyone. So the client receives a re-opened O_RDONLY view of the same
    * memfd — mmap PROT_WRITE on it fails — while the host keeps its own
    * read-write fd to write heartbeats and bump the epoch. The arena and the
    * client's own queue pair are legitimately read-write. */
   char proc[64];
   snprintf(proc, sizeof proc, "/proc/self/fd/%d", h->control_fd);
   int control_ro = open(proc, O_RDONLY | O_CLOEXEC);
   if (control_ro < 0)
   {
      slot_release(h, idx);
      return deny(conn_fd, BUS_ATTACH_DENIED_NOSLOT);
   }

   int gfds[3] = {control_ro, h->arena_fd, s->qpair_fd};
   int rc = bus_fd_send(conn_fd, &reply, sizeof reply, gfds, 3);
   close(control_ro); /* the client holds its own dup now */
   if (rc != 0)
   {
      /* The client never received its grant; undo the admission. */
      slot_release(h, idx);
      return BUS_HOST_ERR_OS;
   }
   if (slot_out)
      *slot_out = idx;
   return BUS_HOST_OK;
}

/* ------------------------------------------------------------------ */
/* reaping                                                             */

uint32_t bus_host_reap(bus_host_t *h, uint64_t now, uint64_t stale_ns)
{
   if (!h || !h->slots)
      return 0;
   uint32_t reaped = 0;
   for (uint32_t i = 0; i < h->cfg.max_slots; i++)
   {
      bus_slot_t *s = &h->slots[i];
      if (!s->in_use)
         continue;

      uint64_t hb = atomic_load_explicit(&s->qpair.hdr->client_heartbeat, memory_order_acquire);
      if (hb != s->last_heartbeat)
      {
         /* The client is alive; note the advance. */
         s->last_heartbeat = hb;
         s->heartbeat_at = now;
         continue;
      }
      /* No advance since we last looked. A slot never yet seen advancing uses
       * its admission time as the baseline (heartbeat_at was set to 0 at
       * admission, so treat 0 as "start the clock now" the first time). */
      if (s->heartbeat_at == 0)
      {
         s->heartbeat_at = now;
         continue;
      }
      if (now - s->heartbeat_at >= stale_ns)
      {
         slot_release(h, i);
         reaped++;
      }
   }
   return reaped;
}

void bus_host_bump_epoch(bus_host_t *h)
{
   if (h && h->control)
      bus_control_bump_epoch(h->control);
}

uint32_t bus_host_admitted(const bus_host_t *h)
{
   return h ? h->admitted : 0;
}

void bus_host_set_attach_hook(bus_host_t *h,
                              bus_attach_status_t (*hook)(void *ctx, bus_host_t *host,
                                                          uint32_t slot,
                                                          const bus_attach_request_t *request),
                              void *ctx)
{
   if (!h)
      return;
   h->attach_hook = hook;
   h->attach_hook_ctx = hook ? ctx : NULL;
}

const char *bus_host_result_name(bus_host_result_t r)
{
   switch (r)
   {
   case BUS_HOST_OK:
      return "OK";
   case BUS_HOST_ERR_ARG:
      return "ERR_ARG";
   case BUS_HOST_ERR_OS:
      return "ERR_OS";
   case BUS_HOST_ERR_REGION:
      return "ERR_REGION";
   case BUS_HOST_ERR_REFUSED:
      return "ERR_REFUSED";
   default:
      return "ERR_UNKNOWN";
   }
}

const char *bus_attach_status_name(bus_attach_status_t s)
{
   switch (s)
   {
   case BUS_ATTACH_OK:
      return "OK";
   case BUS_ATTACH_DENIED_POLICY:
      return "DENIED_POLICY";
   case BUS_ATTACH_DENIED_VERSION:
      return "DENIED_VERSION";
   case BUS_ATTACH_DENIED_NOSLOT:
      return "DENIED_NOSLOT";
   case BUS_ATTACH_PROTOCOL:
      return "PROTOCOL";
   default:
      return "UNKNOWN";
   }
}
