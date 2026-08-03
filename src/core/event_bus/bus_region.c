/* bus_region.c: client-safe mapping and validation. See bus_region.h. */
#include <sys/mman.h>
#include <sys/stat.h>

#include <aimee/core/event_bus/bus_region.h>

#include "bus_region_internal.h"

bus_region_result_t bus_region_map(int fd, size_t size, int writable, bus_region_t *out)
{
   if (!out || fd < 0 || size == 0)
      return BUS_REGION_ERR_ARG;

   /* mmap will happily map past the end of the backing memfd; touching those
    * pages then raises SIGBUS, and every later bounds check that trusts r->size
    * would have approved a header whose backing object is shorter. So the
    * mapping may not exceed the fd's actual length — verified before mmap, not
    * discovered by a fault later. */
   struct stat st;
   if (fstat(fd, &st) != 0)
      return BUS_REGION_ERR_OS;
   if (st.st_size < 0 || size > (size_t)st.st_size)
      return BUS_REGION_ERR_SIZE;

   int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
   void *base = mmap(NULL, size, prot, MAP_SHARED, fd, 0);
   if (base == MAP_FAILED)
      return BUS_REGION_ERR_OS;

   out->fd = fd;
   out->base = base;
   out->size = size;
   out->writable = writable ? 1 : 0;
   return BUS_REGION_OK;
}

void bus_region_unmap(bus_region_t *r)
{
   if (r && r->base)
   {
      munmap(r->base, r->size);
      r->base = NULL;
   }
}

/* ------------------------------------------------------------------ */
/* control region                                                      */

size_t bus_control_bytes(void)
{
   return sizeof(bus_control_t);
}

bus_region_result_t bus_control_attach(const bus_region_t *r, bus_control_t **out)
{
   if (!r || !r->base || !out)
      return BUS_REGION_ERR_ARG;
   if (r->size < sizeof(bus_control_t))
      return BUS_REGION_ERR_SIZE;

   bus_control_t *c = (bus_control_t *)r->base;
   if (atomic_load_explicit(&c->magic, memory_order_acquire) != BUS_CONTROL_MAGIC)
      return BUS_REGION_ERR_MAGIC;
   if (atomic_load_explicit(&c->spec_version, memory_order_acquire) != BUS_SPEC_VERSION ||
       atomic_load_explicit(&c->layout_version, memory_order_acquire) != BUS_LAYOUT_VERSION)
      return BUS_REGION_ERR_VERSION;

   /* The D4 parameters are a claim; a client will size mappings against them, so
    * a header describing nonsense must be refused rather than propagated. Every
    * value a mapping or a ring walk is later sized on is validated here,
    * including queue_capacity (which sizes the queue-pair rings) — an omission
    * the first review caught. */
   uint32_t slot = atomic_load_explicit(&c->slot_size, memory_order_acquire);
   uint32_t inl = atomic_load_explicit(&c->inline_budget, memory_order_acquire);
   uint32_t qcap = atomic_load_explicit(&c->queue_capacity, memory_order_acquire);
   uint64_t arena = atomic_load_explicit(&c->arena_size, memory_order_acquire);
   if (slot == 0 || inl > slot || arena == 0 || arena > BUS_ARENA_MAX_SIZE)
      return BUS_REGION_ERR_GEOMETRY;
   if (bus_qpair_bytes(slot, qcap) == 0)
      return BUS_REGION_ERR_GEOMETRY;

   *out = c;
   return BUS_REGION_OK;
}

uint64_t bus_control_epoch(const bus_control_t *c)
{
   return c ? atomic_load_explicit(&c->host_epoch, memory_order_acquire) : 0;
}

int bus_control_epoch_changed(const bus_control_t *c, uint64_t attached_epoch)
{
   return c && atomic_load_explicit(&c->host_epoch, memory_order_acquire) != attached_epoch;
}

/* ------------------------------------------------------------------ */
/* queue-pair region                                                   */

size_t bus_qpair_bytes(uint32_t slot_size, uint32_t capacity)
{
   size_t ring = bus_ring_bytes(slot_size, capacity);
   if (ring == 0)
      return 0;
   size_t out_off = bus_qpair_outbound_offset(slot_size, capacity);
   if (out_off == 0)
      return 0;
   return out_off + ring;
}

bus_region_result_t bus_qpair_attach(const bus_region_t *r, bus_qpair_t *out)
{
   if (!r || !r->base || !out)
      return BUS_REGION_ERR_ARG;
   if (r->size < sizeof(bus_qpair_hdr_t))
      return BUS_REGION_ERR_SIZE;

   bus_qpair_hdr_t *h = (bus_qpair_hdr_t *)r->base;
   if (atomic_load_explicit(&h->magic, memory_order_acquire) != BUS_QPAIR_MAGIC)
      return BUS_REGION_ERR_MAGIC;

   uint32_t slot = atomic_load_explicit(&h->slot_size, memory_order_acquire);
   uint32_t cap = atomic_load_explicit(&h->capacity, memory_order_acquire);
   uint32_t in_off = atomic_load_explicit(&h->inbound_off, memory_order_acquire);
   uint32_t out_off = atomic_load_explicit(&h->outbound_off, memory_order_acquire);

   /* Everything is a claim. The offsets and geometry must place two whole rings
    * inside the mapping, or a later hot-path access walks off the end. */
   size_t need = bus_qpair_bytes(slot, cap);
   if (need == 0 || r->size < need)
      return BUS_REGION_ERR_GEOMETRY;
   if (in_off != bus_qpair_inbound_offset() || out_off != bus_qpair_outbound_offset(slot, cap))
      return BUS_REGION_ERR_GEOMETRY;

   uint8_t *b = (uint8_t *)r->base;
   bus_ring_result_t rr;
   rr = bus_ring_attach(b + in_off, r->size - in_off, &out->inbound);
   if (rr != BUS_RING_OK)
      return BUS_REGION_ERR_GEOMETRY;
   rr = bus_ring_attach(b + out_off, r->size - out_off, &out->outbound);
   if (rr != BUS_RING_OK)
      return BUS_REGION_ERR_GEOMETRY;

   out->hdr = h;
   return BUS_REGION_OK;
}

/* ------------------------------------------------------------------ */
/* arena region                                                        */

size_t bus_arena_region_bytes(uint64_t arena_size)
{
   if (arena_size == 0 || arena_size > BUS_ARENA_MAX_SIZE)
      return 0;
   return sizeof(bus_arena_hdr_t) + (size_t)arena_size;
}

bus_region_result_t bus_arena_region_attach(const bus_region_t *r, uint8_t **base, uint64_t *size)
{
   if (!r || !r->base || !base || !size)
      return BUS_REGION_ERR_ARG;
   if (r->size < sizeof(bus_arena_hdr_t))
      return BUS_REGION_ERR_SIZE;

   bus_arena_hdr_t *h = (bus_arena_hdr_t *)r->base;
   if (atomic_load_explicit(&h->magic, memory_order_acquire) != BUS_ARENA_MAGIC)
      return BUS_REGION_ERR_MAGIC;

   uint64_t arena = atomic_load_explicit(&h->size, memory_order_acquire);
   if (arena == 0 || arena > BUS_ARENA_MAX_SIZE)
      return BUS_REGION_ERR_GEOMETRY;
   if (r->size < sizeof(bus_arena_hdr_t) + arena)
      return BUS_REGION_ERR_SIZE;

   *base = (uint8_t *)r->base + sizeof(bus_arena_hdr_t);
   *size = arena;
   return BUS_REGION_OK;
}

const char *bus_region_result_name(bus_region_result_t r)
{
   switch (r)
   {
   case BUS_REGION_OK:
      return "OK";
   case BUS_REGION_ERR_ARG:
      return "ERR_ARG";
   case BUS_REGION_ERR_OS:
      return "ERR_OS";
   case BUS_REGION_ERR_SIZE:
      return "ERR_SIZE";
   case BUS_REGION_ERR_MAGIC:
      return "ERR_MAGIC";
   case BUS_REGION_ERR_VERSION:
      return "ERR_VERSION";
   case BUS_REGION_ERR_GEOMETRY:
      return "ERR_GEOMETRY";
   case BUS_REGION_ERR_EPOCH:
      return "ERR_EPOCH";
   default:
      return "ERR_UNKNOWN";
   }
}
