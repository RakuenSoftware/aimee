/* bus_ring.c: SPSC ring buffer. See bus_ring.h for the concurrency contract.
 *
 * The memory-ordering argument, once, because it is the whole correctness case:
 *
 *   Producer: fills slot[head & mask], then stores head+1 with release.
 *   Consumer: loads head with acquire, then reads slot[tail & mask].
 *
 * The release/acquire pair on `head` orders the slot writes before the index
 * that publishes them, so a consumer that sees the new head is guaranteed to
 * see the bytes. The mirror pair on `tail` does the same for reuse: the
 * producer must not overwrite a slot until it has acquired the tail that
 * released it.
 *
 * Each side loads its *own* index relaxed — nobody else writes it, so there is
 * nothing to synchronise with — and the other side's index with acquire.
 */
#include <string.h>

#include "bus_ring.h"

/* Bound the geometry so a corrupt or hostile header cannot ask for an
 * allocation that overflows the size computation below. */
#define BUS_RING_MAX_SLOT_SIZE (1u << 24)
#define BUS_RING_MAX_CAPACITY  (1u << 20)

static int is_pow2(uint32_t v)
{
   return v != 0 && (v & (v - 1)) == 0;
}

static size_t header_bytes(void)
{
   /* offsetof(slots) already includes the alignment padding the compiler
    * inserted for the cache-line-aligned members. */
   return offsetof(bus_ring_t, slots);
}

size_t bus_ring_bytes(uint32_t slot_size, uint32_t capacity)
{
   if (slot_size == 0 || slot_size > BUS_RING_MAX_SLOT_SIZE)
      return 0;
   if (!is_pow2(capacity) || capacity < BUS_RING_MIN_CAPACITY ||
       capacity > BUS_RING_MAX_CAPACITY)
      return 0;

   /* Both operands are bounded above, so this cannot overflow size_t on any
    * platform with a 64-bit size_t; the check is kept for the 32-bit case. */
   size_t slots = (size_t)slot_size * (size_t)capacity;
   if (slots / capacity != slot_size)
      return 0;

   size_t total = header_bytes() + slots;
   if (total < slots)
      return 0;
   return total;
}

bus_ring_result_t bus_ring_init(void *mem, size_t memsz, uint32_t slot_size, uint32_t capacity,
                                bus_ring_t **out)
{
   if (!mem || !out)
      return BUS_RING_ERR_MEM;

   size_t need = bus_ring_bytes(slot_size, capacity);
   if (need == 0)
      return BUS_RING_ERR_GEOMETRY;
   if (memsz < need)
      return BUS_RING_ERR_MEM;

   bus_ring_t *r = (bus_ring_t *)mem;
   memset(mem, 0, need);
   r->slot_size = slot_size;
   r->capacity = capacity;
   r->mask = capacity - 1;
   r->slots_off = (uint32_t)header_bytes();
   atomic_store_explicit(&r->head, 0, memory_order_relaxed);
   atomic_store_explicit(&r->tail, 0, memory_order_relaxed);

   /* Magic last, with a release store: a peer that sees the magic is guaranteed
    * to see the fully initialised header behind it. Writing it first would
    * expose a half-built ring to anyone already polling for it. */
   atomic_store_explicit(&r->magic, BUS_RING_MAGIC, memory_order_release);

   *out = r;
   return BUS_RING_OK;
}

bus_ring_result_t bus_ring_attach(void *mem, size_t memsz, bus_ring_t **out)
{
   if (!mem || !out)
      return BUS_RING_ERR_MEM;
   if (memsz < header_bytes())
      return BUS_RING_ERR_MEM;

   bus_ring_t *r = (bus_ring_t *)mem;
   if (atomic_load_explicit(&r->magic, memory_order_acquire) != BUS_RING_MAGIC)
      return BUS_RING_ERR_MAGIC;

   /* The header was written by another process. Treat every field as a claim
    * to be checked against the buffer actually mapped, not as a fact: a header
    * describing a larger ring than was mapped would otherwise turn into reads
    * past the end of the mapping. */
   if (!is_pow2(r->capacity) || r->capacity < BUS_RING_MIN_CAPACITY ||
       r->capacity > BUS_RING_MAX_CAPACITY)
      return BUS_RING_ERR_GEOMETRY;
   if (r->slot_size == 0 || r->slot_size > BUS_RING_MAX_SLOT_SIZE)
      return BUS_RING_ERR_GEOMETRY;
   if (r->mask != r->capacity - 1)
      return BUS_RING_ERR_LAYOUT;
   if (r->slots_off != header_bytes())
      return BUS_RING_ERR_LAYOUT;

   size_t need = bus_ring_bytes(r->slot_size, r->capacity);
   if (need == 0)
      return BUS_RING_ERR_GEOMETRY;
   if (memsz < need)
      return BUS_RING_ERR_LAYOUT;

   /* An index pair that claims more entries than the ring can hold is either
    * corruption or a hostile write; either way it must not become an
    * out-of-bounds slot index. */
   uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
   uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
   if (head - tail > r->capacity)
      return BUS_RING_ERR_LAYOUT;

   *out = r;
   return BUS_RING_OK;
}

static uint8_t *slot_at(bus_ring_t *r, uint64_t index)
{
   return r->slots + (size_t)(index & r->mask) * r->slot_size;
}

void *bus_ring_produce_begin(bus_ring_t *r)
{
   if (!r)
      return NULL;

   /* Our own index: nobody else writes it, so relaxed is enough. */
   uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
   /* The consumer's index: acquire, so slots it released are safe to reuse. */
   uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

   if (head - tail >= r->capacity)
      return NULL; /* full */

   return slot_at(r, head);
}

void bus_ring_produce_commit(bus_ring_t *r)
{
   if (!r)
      return;
   uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
   /* Release: everything written into the slot happens-before the consumer's
    * acquire load of this index. */
   atomic_store_explicit(&r->head, head + 1, memory_order_release);
}

const void *bus_ring_consume_begin(const bus_ring_t *r)
{
   if (!r)
      return NULL;

   uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
   uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);

   if (head == tail)
      return NULL; /* empty */

   /* Casting away const is confined to this line. The consumer does not write
    * the slot; the interface returns const, and only the address arithmetic
    * needs a mutable base. */
   bus_ring_t *m = (bus_ring_t *)(uintptr_t)r;
   return slot_at(m, tail);
}

void bus_ring_consume_commit(bus_ring_t *r)
{
   if (!r)
      return;
   uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
   /* Release: our reads of the slot happen-before the producer's acquire load
    * of this index, so it cannot overwrite bytes we are still reading. */
   atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
}

uint64_t bus_ring_count(const bus_ring_t *r)
{
   if (!r)
      return 0;
   uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
   uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
   return head - tail;
}

uint32_t bus_ring_capacity(const bus_ring_t *r)
{
   return r ? r->capacity : 0;
}

const char *bus_ring_result_name(bus_ring_result_t r)
{
   switch (r)
   {
   case BUS_RING_OK:
      return "OK";
   case BUS_RING_ERR_MEM:
      return "ERR_MEM";
   case BUS_RING_ERR_GEOMETRY:
      return "ERR_GEOMETRY";
   case BUS_RING_ERR_MAGIC:
      return "ERR_MAGIC";
   case BUS_RING_ERR_LAYOUT:
      return "ERR_LAYOUT";
   default:
      return "ERR_UNKNOWN";
   }
}
