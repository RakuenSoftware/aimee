/* Host-only creation and initialisation for event-bus shared-memory regions. */
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <aimee/core/event_bus/bus_region_host.h>

#include "bus_region_internal.h"

static size_t page_round_up(size_t n)
{
   long ps = sysconf(_SC_PAGESIZE);
   size_t page = (ps > 0) ? (size_t)ps : 4096;
   size_t rounded = (n + page - 1) & ~(page - 1);
   if (rounded < n)
      return 0;
   return rounded;
}

bus_region_result_t bus_region_create(const char *name, size_t size, bus_region_t *out)
{
   if (!out || size == 0)
      return BUS_REGION_ERR_ARG;

   size_t rounded = page_round_up(size);
   if (rounded == 0)
      return BUS_REGION_ERR_SIZE;

   int fd = memfd_create(name ? name : "aimee-bus", MFD_CLOEXEC);
   if (fd < 0)
      return BUS_REGION_ERR_OS;
   if (ftruncate(fd, (off_t)rounded) != 0)
   {
      int saved = errno;
      close(fd);
      errno = saved;
      return BUS_REGION_ERR_OS;
   }

   out->fd = fd;
   out->base = NULL;
   out->size = rounded;
   out->writable = 0;
   return BUS_REGION_OK;
}

bus_region_result_t bus_control_init(bus_region_t *r, uint32_t slot_size, uint32_t inline_budget,
                                     uint32_t queue_capacity, uint64_t arena_size)
{
   if (!r || !r->base || !r->writable)
      return BUS_REGION_ERR_ARG;
   if (r->size < sizeof(bus_control_t))
      return BUS_REGION_ERR_SIZE;
   if (slot_size == 0 || inline_budget > slot_size || arena_size == 0 ||
       arena_size > BUS_ARENA_MAX_SIZE || bus_qpair_bytes(slot_size, queue_capacity) == 0)
      return BUS_REGION_ERR_GEOMETRY;

   bus_control_t *c = (bus_control_t *)r->base;
   memset(c, 0, sizeof *c);
   atomic_store_explicit(&c->spec_version, BUS_SPEC_VERSION, memory_order_relaxed);
   atomic_store_explicit(&c->layout_version, BUS_LAYOUT_VERSION, memory_order_relaxed);
   atomic_store_explicit(&c->flags, 0, memory_order_relaxed);
   atomic_store_explicit(&c->slot_size, slot_size, memory_order_relaxed);
   atomic_store_explicit(&c->inline_budget, inline_budget, memory_order_relaxed);
   atomic_store_explicit(&c->queue_capacity, queue_capacity, memory_order_relaxed);
   atomic_store_explicit(&c->arena_size, arena_size, memory_order_relaxed);
   atomic_store_explicit(&c->host_epoch, 1, memory_order_relaxed);
   atomic_store_explicit(&c->host_heartbeat, 0, memory_order_relaxed);
   atomic_store_explicit(&c->magic, BUS_CONTROL_MAGIC, memory_order_release);
   return BUS_REGION_OK;
}

void bus_control_bump_epoch(bus_control_t *c)
{
   if (c)
      atomic_fetch_add_explicit(&c->host_epoch, 1, memory_order_acq_rel);
}

void bus_control_heartbeat(bus_control_t *c, uint64_t now)
{
   if (c)
      atomic_store_explicit(&c->host_heartbeat, now, memory_order_release);
}

bus_region_result_t bus_qpair_init(bus_region_t *r, uint32_t slot_size, uint32_t capacity)
{
   if (!r || !r->base || !r->writable)
      return BUS_REGION_ERR_ARG;
   size_t need = bus_qpair_bytes(slot_size, capacity);
   if (need == 0)
      return BUS_REGION_ERR_GEOMETRY;
   if (r->size < need)
      return BUS_REGION_ERR_SIZE;

   uint8_t *b = (uint8_t *)r->base;
   bus_qpair_hdr_t *h = (bus_qpair_hdr_t *)b;
   memset(h, 0, sizeof *h);
   atomic_store_explicit(&h->slot_size, slot_size, memory_order_relaxed);
   atomic_store_explicit(&h->capacity, capacity, memory_order_relaxed);
   atomic_store_explicit(&h->data_credits, capacity, memory_order_relaxed);
   atomic_store_explicit(&h->control_credits, BUS_CONTROL_CREDITS_DEFAULT, memory_order_relaxed);
   atomic_store_explicit(&h->control_lost, 0, memory_order_relaxed);
   atomic_store_explicit(&h->inbound_off, (uint32_t)bus_qpair_inbound_offset(),
                         memory_order_relaxed);
   atomic_store_explicit(&h->outbound_off, (uint32_t)bus_qpair_outbound_offset(slot_size, capacity),
                         memory_order_relaxed);
   atomic_store_explicit(&h->client_heartbeat, 0, memory_order_relaxed);

   bus_ring_t tmp;
   bus_ring_result_t rr =
       bus_ring_init(b + bus_qpair_inbound_offset(), r->size - bus_qpair_inbound_offset(),
                     slot_size, capacity, &tmp);
   if (rr != BUS_RING_OK)
      return BUS_REGION_ERR_GEOMETRY;
   size_t out_off = bus_qpair_outbound_offset(slot_size, capacity);
   rr = bus_ring_init(b + out_off, r->size - out_off, slot_size, capacity, &tmp);
   if (rr != BUS_RING_OK)
      return BUS_REGION_ERR_GEOMETRY;

   atomic_store_explicit(&h->magic, BUS_QPAIR_MAGIC, memory_order_release);
   return BUS_REGION_OK;
}

bus_region_result_t bus_arena_region_init(bus_region_t *r, uint64_t arena_size)
{
   if (!r || !r->base || !r->writable)
      return BUS_REGION_ERR_ARG;
   size_t need = bus_arena_region_bytes(arena_size);
   if (need == 0)
      return BUS_REGION_ERR_GEOMETRY;
   if (r->size < need)
      return BUS_REGION_ERR_SIZE;

   bus_arena_hdr_t *h = (bus_arena_hdr_t *)r->base;
   memset(h, 0, sizeof *h);
   atomic_store_explicit(&h->size, arena_size, memory_order_relaxed);
   atomic_store_explicit(&h->magic, BUS_ARENA_MAGIC, memory_order_release);
   return BUS_REGION_OK;
}
