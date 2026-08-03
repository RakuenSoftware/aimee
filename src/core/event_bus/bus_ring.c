/* bus_ring.c: SPSC ring buffer. See bus_ring.h for the concurrency contract and
 * for why geometry is copied into a process-local handle rather than re-read
 * from shared memory.
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

#include <aimee/core/event_bus/bus_ring.h>

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
   return offsetof(bus_ring_shared_t, slots);
}

size_t bus_ring_bytes(uint32_t slot_size, uint32_t capacity)
{
   if (slot_size == 0 || slot_size > BUS_RING_MAX_SLOT_SIZE)
      return 0;
   if (!is_pow2(capacity) || capacity < BUS_RING_MIN_CAPACITY || capacity > BUS_RING_MAX_CAPACITY)
      return 0;

   size_t slots = (size_t)slot_size * (size_t)capacity;
   if (slots / capacity != slot_size)
      return 0;

   size_t total = header_bytes() + slots;
   if (total < slots)
      return 0;
   return total;
}

/* Fill a handle from geometry that has already been validated. One place, so
 * init and attach cannot disagree about what a handle means. */
static void handle_from(bus_ring_t *out, bus_ring_shared_t *s, uint32_t slot_size,
                        uint32_t capacity, uint32_t slots_off)
{
   out->shared = s;
   out->slots = (uint8_t *)s + slots_off;
   out->slot_size = slot_size;
   out->capacity = capacity;
   out->mask = capacity - 1;
}

bus_ring_result_t bus_ring_init(void *mem, size_t memsz, uint32_t slot_size, uint32_t capacity,
                                bus_ring_t *out)
{
   if (!mem || !out)
      return BUS_RING_ERR_MEM;

   size_t need = bus_ring_bytes(slot_size, capacity);
   if (need == 0)
      return BUS_RING_ERR_GEOMETRY;
   if (memsz < need)
      return BUS_RING_ERR_MEM;

   bus_ring_shared_t *s = (bus_ring_shared_t *)mem;
   memset(mem, 0, need);
   atomic_store_explicit(&s->slot_size, slot_size, memory_order_relaxed);
   atomic_store_explicit(&s->capacity, capacity, memory_order_relaxed);
   atomic_store_explicit(&s->mask, capacity - 1, memory_order_relaxed);
   atomic_store_explicit(&s->slots_off, (uint32_t)header_bytes(), memory_order_relaxed);
   atomic_store_explicit(&s->head, 0, memory_order_relaxed);
   atomic_store_explicit(&s->tail, 0, memory_order_relaxed);

   /* Magic last, with a release store: a peer that sees the magic is guaranteed
    * to see the fully initialised header behind it. Writing it first would
    * expose a half-built ring to anyone already polling for it. */
   atomic_store_explicit(&s->magic, BUS_RING_MAGIC, memory_order_release);

   handle_from(out, s, slot_size, capacity, (uint32_t)header_bytes());
   return BUS_RING_OK;
}

bus_ring_result_t bus_ring_attach(void *mem, size_t memsz, bus_ring_t *out)
{
   if (!mem || !out)
      return BUS_RING_ERR_MEM;
   if (memsz < header_bytes())
      return BUS_RING_ERR_MEM;

   bus_ring_shared_t *s = (bus_ring_shared_t *)mem;
   if (atomic_load_explicit(&s->magic, memory_order_acquire) != BUS_RING_MAGIC)
      return BUS_RING_ERR_MAGIC;

   /* Each geometry field is read EXACTLY ONCE, into a local. Re-reading any of
    * them after validation would let a peer change the value between the check
    * and the use — the whole reason the handle exists. */
   uint32_t slot_size = atomic_load_explicit(&s->slot_size, memory_order_acquire);
   uint32_t capacity = atomic_load_explicit(&s->capacity, memory_order_acquire);
   uint32_t mask = atomic_load_explicit(&s->mask, memory_order_acquire);
   uint32_t slots_off = atomic_load_explicit(&s->slots_off, memory_order_acquire);

   /* Everything below judges those locals. The header is a claim; a header
    * describing a larger ring than was mapped would otherwise turn into reads
    * past the end of the mapping. */
   if (!is_pow2(capacity) || capacity < BUS_RING_MIN_CAPACITY || capacity > BUS_RING_MAX_CAPACITY)
      return BUS_RING_ERR_GEOMETRY;
   if (slot_size == 0 || slot_size > BUS_RING_MAX_SLOT_SIZE)
      return BUS_RING_ERR_GEOMETRY;
   if (mask != capacity - 1)
      return BUS_RING_ERR_LAYOUT;
   if (slots_off != header_bytes())
      return BUS_RING_ERR_LAYOUT;

   size_t need = bus_ring_bytes(slot_size, capacity);
   if (need == 0)
      return BUS_RING_ERR_GEOMETRY;
   if (memsz < need)
      return BUS_RING_ERR_LAYOUT;

   /* An index pair claiming more entries than the ring can hold is corruption.
    * Reading the two counters is only meaningful because attach is an
    * initialization-phase operation with no concurrent traffic (see the header):
    * during traffic these two loads would not form a coherent snapshot and a
    * healthy ring could be rejected. `> capacity` and not `>=`, because a
    * difference of exactly capacity is the ordinary full state. */
   uint64_t head = atomic_load_explicit(&s->head, memory_order_acquire);
   uint64_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
   if (head - tail > capacity)
      return BUS_RING_ERR_LAYOUT;

   handle_from(out, s, slot_size, capacity, slots_off);
   return BUS_RING_OK;
}

/* Addressing uses only the handle's validated copies, so no value a peer can
 * write to the shared header can move where this process reads or writes. */
static uint8_t *slot_at(const bus_ring_t *r, uint64_t index)
{
   return r->slots + (size_t)(index & r->mask) * r->slot_size;
}

void *bus_ring_produce_begin(const bus_ring_t *r)
{
   if (!r || !r->shared)
      return NULL;

   /* Our own index: nobody else writes it, so relaxed is enough. */
   uint64_t head = atomic_load_explicit(&r->shared->head, memory_order_relaxed);
   /* The consumer's index: acquire, so slots it released are safe to reuse. */
   uint64_t tail = atomic_load_explicit(&r->shared->tail, memory_order_acquire);

   if (head - tail >= r->capacity)
      return NULL; /* full */

   return slot_at(r, head);
}

void bus_ring_produce_commit(const bus_ring_t *r)
{
   if (!r || !r->shared)
      return;
   uint64_t head = atomic_load_explicit(&r->shared->head, memory_order_relaxed);
   /* Release: everything written into the slot happens-before the consumer's
    * acquire load of this index. */
   atomic_store_explicit(&r->shared->head, head + 1, memory_order_release);
}

const void *bus_ring_consume_begin(const bus_ring_t *r)
{
   if (!r || !r->shared)
      return NULL;

   uint64_t tail = atomic_load_explicit(&r->shared->tail, memory_order_relaxed);
   uint64_t head = atomic_load_explicit(&r->shared->head, memory_order_acquire);

   if (head == tail)
      return NULL; /* empty */

   return slot_at(r, tail);
}

void bus_ring_consume_commit(const bus_ring_t *r)
{
   if (!r || !r->shared)
      return;
   uint64_t tail = atomic_load_explicit(&r->shared->tail, memory_order_relaxed);
   /* Release: our reads of the slot happen-before the producer's acquire load
    * of this index, so it cannot overwrite bytes we are still reading. */
   atomic_store_explicit(&r->shared->tail, tail + 1, memory_order_release);
}

uint64_t bus_ring_count(const bus_ring_t *r)
{
   if (!r || !r->shared)
      return 0;
   uint64_t head = atomic_load_explicit(&r->shared->head, memory_order_acquire);
   uint64_t tail = atomic_load_explicit(&r->shared->tail, memory_order_acquire);
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
