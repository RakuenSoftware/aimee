/* bus_arena.c: host-mediated lease allocator. See bus_arena.h for the D3 model.
 *
 * The allocator is a first-fit free list with coalescing. Coalescing on every
 * free is what keeps churn from stranding the arena: two frees that touch merge,
 * so after everything is released the arena is one span again. All of this
 * bookkeeping is host-private — the shared arena holds only payload bytes.
 *
 * Concurrency: the table is guarded by a->lock (see bus_arena.h). Each public
 * entry point takes the lock, runs a `_locked` core that assumes it is held, and
 * drops it. The static helpers below (free list, lease helpers) are only ever
 * called from a `_locked` core, so they never touch the lock themselves. A
 * pointer handed back by fill_ptr/read_ptr is dereferenced by the caller AFTER
 * the lock is dropped — that is safe because a live ref keeps the span alive, and
 * the lock only ever protected the table, never the payload bytes.
 */
#include <string.h>

#include <aimee/core/event_bus/bus_arena.h>

static void arena_lock(const bus_arena_t *a)
{
   /* Logically const: the lock is bookkeeping, not part of the observed value.
    * The observers take a const arena but must still serialise against writers. */
   pthread_mutex_lock(&((bus_arena_t *)a)->lock);
}
static void arena_unlock(const bus_arena_t *a)
{
   pthread_mutex_unlock(&((bus_arena_t *)a)->lock);
}

/* Alignment so consecutive leases do not share a machine word, and lengths are
 * rounded up predictably. */
#define ARENA_ALIGN 16u

static uint64_t align_up(uint64_t n)
{
   return (n + (ARENA_ALIGN - 1)) & ~((uint64_t)ARENA_ALIGN - 1);
}

/* ---- slot bitmaps ---- */

static void bits_set(uint64_t *bits, uint32_t slot)
{
   bits[slot / 64] |= (uint64_t)1 << (slot % 64);
}
static void bits_clear(uint64_t *bits, uint32_t slot)
{
   bits[slot / 64] &= ~((uint64_t)1 << (slot % 64));
}
static int bits_test(const uint64_t *bits, uint32_t slot)
{
   return (bits[slot / 64] >> (slot % 64)) & 1;
}
static uint32_t bits_popcount(const uint64_t *bits)
{
   uint32_t n = 0;
   for (int i = 0; i < BUS_ARENA_SLOT_WORDS; i++)
   {
      uint64_t w = bits[i];
      while (w)
      {
         w &= w - 1;
         n++;
      }
   }
   return n;
}

/* ---- free list ---- */

/* Insert [off,len) keeping the list sorted by offset, then coalesce neighbours.
 *
 * The array holds BUS_ARENA_MAX_LEASES + 1 spans, which cannot overflow: free
 * and allocated spans strictly alternate along the arena, so with at most
 * MAX_LEASES allocated spans there are at most MAX_LEASES + 1 free spans, and
 * the transient +1 during a shift-then-coalesce insert is exactly the spare
 * slot. This runs after a free (which never raises the live-lease count), so the
 * pre-insert free_count is always < the array bound. */
static void free_insert(bus_arena_t *a, uint64_t off, uint64_t len)
{
   uint32_t i = 0;
   while (i < a->free_count && a->freelist[i].offset < off)
      i++;

   /* shift up to make room */
   for (uint32_t j = a->free_count; j > i; j--)
      a->freelist[j] = a->freelist[j - 1];
   a->freelist[i].offset = off;
   a->freelist[i].len = len;
   a->free_count++;

   /* coalesce with the previous span if adjacent */
   if (i > 0 && a->freelist[i - 1].offset + a->freelist[i - 1].len == a->freelist[i].offset)
   {
      a->freelist[i - 1].len += a->freelist[i].len;
      for (uint32_t j = i; j + 1 < a->free_count; j++)
         a->freelist[j] = a->freelist[j + 1];
      a->free_count--;
      i--;
   }
   /* coalesce with the next span if adjacent */
   if (i + 1 < a->free_count &&
       a->freelist[i].offset + a->freelist[i].len == a->freelist[i + 1].offset)
   {
      a->freelist[i].len += a->freelist[i + 1].len;
      for (uint32_t j = i + 1; j + 1 < a->free_count; j++)
         a->freelist[j] = a->freelist[j + 1];
      a->free_count--;
   }
}

/* First-fit carve of `len` bytes; returns 1 and sets *off on success. */
static int free_take(bus_arena_t *a, uint64_t len, uint64_t *off)
{
   for (uint32_t i = 0; i < a->free_count; i++)
   {
      if (a->freelist[i].len >= len)
      {
         *off = a->freelist[i].offset;
         a->freelist[i].offset += len;
         a->freelist[i].len -= len;
         if (a->freelist[i].len == 0)
         {
            for (uint32_t j = i; j + 1 < a->free_count; j++)
               a->freelist[j] = a->freelist[j + 1];
            a->free_count--;
         }
         return 1;
      }
   }
   return 0;
}

/* ---- init ---- */

bus_arena_result_t bus_arena_init(bus_arena_t *a, uint8_t *base, uint64_t size, uint32_t max_slots,
                                  uint32_t per_client_cap)
{
   if (!a || !base || size == 0 || max_slots == 0 || max_slots > BUS_ARENA_MAX_SLOTS ||
       per_client_cap == 0)
      return BUS_ARENA_ERR_ARG;

   memset(a, 0, sizeof *a);
   a->base = base;
   a->size = size;
   a->max_slots = max_slots;
   a->per_client_cap = per_client_cap;
   a->freelist[0].offset = 0;
   a->freelist[0].len = size;
   a->free_count = 1;
   pthread_mutex_init(&a->lock, NULL);
   return BUS_ARENA_OK;
}

void bus_arena_fini(bus_arena_t *a)
{
   if (!a || !a->base)
      return; /* never initialised (or already torn down): nothing to destroy */
   pthread_mutex_destroy(&a->lock);
   a->base = NULL;
}

/* ---- lease helpers (all assume a->lock held) ---- */

static bus_lease_t *lease_at(bus_arena_t *a, uint32_t id)
{
   if (id >= a->lease_count || !a->leases[id].active)
      return NULL;
   return &a->leases[id];
}

static uint32_t lease_refcount(const bus_lease_t *l)
{
   return (uint32_t)l->producer_ref + bits_popcount(l->consumer_bits);
}

/* Free a lease's span and clear the slot for reuse, keeping generation so the
 * next reuse presents a fresh one. */
static void lease_reclaim(bus_arena_t *a, bus_lease_t *l)
{
   free_insert(a, l->offset, l->len);
   a->bytes_in_use -= l->len;
   if (a->live_per_slot[l->owner_slot] > 0)
      a->live_per_slot[l->owner_slot]--;
   uint32_t gen = l->generation;
   memset(l, 0, sizeof *l);
   l->generation = gen; /* preserved: a stale reader must still mismatch */
}

static void lease_drop_if_zero(bus_arena_t *a, bus_lease_t *l)
{
   if (lease_refcount(l) == 0)
      lease_reclaim(a, l);
}

/* ---- alloc ---- */

static bus_arena_result_t bus_arena_alloc_locked(bus_arena_t *a, uint32_t owner_slot, uint32_t len,
                                                 uint32_t *lease_id)
{
   if (!a || !lease_id || owner_slot >= a->max_slots || len == 0)
      return BUS_ARENA_ERR_ARG;

   /* The cap is enforced HERE, synchronously, before any bytes are written —
    * not at reap, which lags behind a heartbeat. */
   if (a->live_per_slot[owner_slot] >= a->per_client_cap)
      return BUS_ARENA_ERR_CAP;

   uint64_t need = align_up(len);
   /* A length near UINT32_MAX aligns past it; a span the arena cannot hold is
    * out of space, not a truncated l->len. Bounding need by the arena size makes
    * both true at once — nothing this large ever fits, and l->len never wraps. */
   if (need == 0 || need > a->size)
      return BUS_ARENA_ERR_NOSPACE;

   /* find a free lease-table slot */
   uint32_t id = BUS_ARENA_MAX_LEASES;
   for (uint32_t i = 0; i < BUS_ARENA_MAX_LEASES; i++)
   {
      if (!a->leases[i].active)
      {
         id = i;
         break;
      }
   }
   if (id == BUS_ARENA_MAX_LEASES)
      return BUS_ARENA_ERR_NOLEASE;

   uint64_t off;
   if (!free_take(a, need, &off))
      return BUS_ARENA_ERR_NOSPACE;

   bus_lease_t *l = &a->leases[id];
   uint32_t gen = l->generation + 1; /* fresh generation for this reuse */
   memset(l, 0, sizeof *l);
   l->active = 1;
   l->producer_ref = 1;
   l->owner_slot = (uint8_t)owner_slot;
   l->generation = gen;
   l->offset = off;
   l->len = (uint32_t)need;

   if (id >= a->lease_count)
      a->lease_count = id + 1;
   a->live_per_slot[owner_slot]++;
   a->bytes_in_use += need;
   *lease_id = id;
   return BUS_ARENA_OK;
}

bus_arena_result_t bus_arena_alloc(bus_arena_t *a, uint32_t owner_slot, uint32_t len,
                                   uint32_t *lease_id)
{
   if (!a)
      return BUS_ARENA_ERR_ARG;
   arena_lock(a);
   bus_arena_result_t r = bus_arena_alloc_locked(a, owner_slot, len, lease_id);
   arena_unlock(a);
   return r;
}

static bus_arena_result_t bus_arena_fill_ptr_locked(bus_arena_t *a, uint32_t lease_id,
                                                    uint8_t **ptr)
{
   if (!a || !ptr)
      return BUS_ARENA_ERR_ARG;
   bus_lease_t *l = lease_at(a, lease_id);
   if (!l)
      return BUS_ARENA_ERR_ARG;
   if (!l->producer_ref)
      return BUS_ARENA_ERR_STATE; /* already published or cancelled */
   *ptr = a->base + l->offset;
   return BUS_ARENA_OK;
}

bus_arena_result_t bus_arena_fill_ptr(bus_arena_t *a, uint32_t lease_id, uint8_t **ptr)
{
   if (!a)
      return BUS_ARENA_ERR_ARG;
   arena_lock(a);
   bus_arena_result_t r = bus_arena_fill_ptr_locked(a, lease_id, ptr);
   arena_unlock(a);
   return r;
}

static bus_arena_result_t bus_arena_ref_locked(const bus_arena_t *a, uint32_t lease_id,
                                               bus_arena_ref_t *ref)
{
   if (!a || !ref)
      return BUS_ARENA_ERR_ARG;
   const bus_lease_t *l =
       (lease_id < a->lease_count && a->leases[lease_id].active) ? &a->leases[lease_id] : NULL;
   if (!l)
      return BUS_ARENA_ERR_ARG;
   ref->offset = l->offset;
   ref->len = l->len;
   ref->generation = l->generation;
   return BUS_ARENA_OK;
}

bus_arena_result_t bus_arena_ref(const bus_arena_t *a, uint32_t lease_id, bus_arena_ref_t *ref)
{
   if (!a)
      return BUS_ARENA_ERR_ARG;
   arena_lock(a);
   bus_arena_result_t r = bus_arena_ref_locked(a, lease_id, ref);
   arena_unlock(a);
   return r;
}

/* ---- publish ---- */

static bus_arena_result_t bus_arena_publish_locked(bus_arena_t *a, uint32_t lease_id,
                                                   const uint8_t *observer_slots, uint32_t k)
{
   if (!a || (k > 0 && !observer_slots))
      return BUS_ARENA_ERR_ARG;
   bus_lease_t *l = lease_at(a, lease_id);
   if (!l)
      return BUS_ARENA_ERR_ARG;
   if (!l->producer_ref)
      return BUS_ARENA_ERR_STATE; /* publish is a once-only transition */

   /* Validate observers first, so a bad list leaves the lease untouched. A slot
    * may not appear twice: catching a duplicate needs a seen-set built as we go,
    * not a test against consumer_bits (which are still zero here) — an idempotent
    * bit-set would otherwise silently collapse [2,2] into a single ref while the
    * caller believed it published to two, and the refcount would be wrong. */
   uint64_t seen[BUS_ARENA_SLOT_WORDS] = {0};
   for (uint32_t i = 0; i < k; i++)
   {
      uint32_t s = observer_slots[i];
      if (s >= a->max_slots)
         return BUS_ARENA_ERR_ARG;
      if (bits_test(seen, s))
         return BUS_ARENA_ERR_ARG; /* a slot may not appear twice */
      bits_set(seen, s);
   }

   for (uint32_t i = 0; i < k; i++)
      bits_set(l->consumer_bits, observer_slots[i]);
   l->producer_ref = 0; /* ownership transfers to the consumers */

   /* Publishing to zero observers takes the refcount straight to zero. */
   lease_drop_if_zero(a, l);
   return BUS_ARENA_OK;
}

bus_arena_result_t bus_arena_publish(bus_arena_t *a, uint32_t lease_id,
                                     const uint8_t *observer_slots, uint32_t k)
{
   if (!a)
      return BUS_ARENA_ERR_ARG;
   arena_lock(a);
   bus_arena_result_t r = bus_arena_publish_locked(a, lease_id, observer_slots, k);
   arena_unlock(a);
   return r;
}

/* ---- consumer read / release ---- */

static bus_arena_result_t bus_arena_read_ptr_locked(const bus_arena_t *a, uint32_t lease_id,
                                                    uint32_t generation, uint32_t consumer_slot,
                                                    const uint8_t **ptr)
{
   if (!a || !ptr || consumer_slot >= a->max_slots)
      return BUS_ARENA_ERR_ARG;
   if (lease_id >= a->lease_count || !a->leases[lease_id].active)
      return BUS_ARENA_ERR_STALE;
   const bus_lease_t *l = &a->leases[lease_id];
   if (l->generation != generation)
      return BUS_ARENA_ERR_STALE;
   if (!bits_test(l->consumer_bits, consumer_slot))
      return BUS_ARENA_ERR_NOTHOLDER;
   *ptr = a->base + l->offset;
   return BUS_ARENA_OK;
}

bus_arena_result_t bus_arena_read_ptr(const bus_arena_t *a, uint32_t lease_id, uint32_t generation,
                                      uint32_t consumer_slot, const uint8_t **ptr)
{
   if (!a)
      return BUS_ARENA_ERR_ARG;
   arena_lock(a);
   bus_arena_result_t r = bus_arena_read_ptr_locked(a, lease_id, generation, consumer_slot, ptr);
   arena_unlock(a);
   return r;
}

static bus_arena_result_t bus_arena_producer_bytes_locked(const bus_arena_t *a, uint32_t lease_id,
                                                          const uint8_t **ptr, uint32_t *len)
{
   if (!a || !ptr || !len)
      return BUS_ARENA_ERR_ARG;
   const bus_lease_t *l =
       (lease_id < a->lease_count && a->leases[lease_id].active) ? &a->leases[lease_id] : NULL;
   if (!l)
      return BUS_ARENA_ERR_ARG;
   if (!l->producer_ref)
      return BUS_ARENA_ERR_STATE; /* published already: the tap ran before routing */
   *ptr = a->base + l->offset;
   *len = l->len;
   return BUS_ARENA_OK;
}

bus_arena_result_t bus_arena_producer_bytes(const bus_arena_t *a, uint32_t lease_id,
                                            const uint8_t **ptr, uint32_t *len)
{
   if (!a)
      return BUS_ARENA_ERR_ARG;
   arena_lock(a);
   bus_arena_result_t r = bus_arena_producer_bytes_locked(a, lease_id, ptr, len);
   arena_unlock(a);
   return r;
}

static bus_arena_result_t bus_arena_release_locked(bus_arena_t *a, uint32_t lease_id,
                                                   uint32_t generation, uint32_t consumer_slot)
{
   if (!a || consumer_slot >= a->max_slots)
      return BUS_ARENA_ERR_ARG;
   if (lease_id >= a->lease_count || !a->leases[lease_id].active)
      return BUS_ARENA_ERR_STALE;
   bus_lease_t *l = &a->leases[lease_id];
   if (l->generation != generation)
      return BUS_ARENA_ERR_STALE;
   if (!bits_test(l->consumer_bits, consumer_slot))
      return BUS_ARENA_ERR_NOTHOLDER;

   bits_clear(l->consumer_bits, consumer_slot);
   lease_drop_if_zero(a, l);
   return BUS_ARENA_OK;
}

bus_arena_result_t bus_arena_release(bus_arena_t *a, uint32_t lease_id, uint32_t generation,
                                     uint32_t consumer_slot)
{
   if (!a)
      return BUS_ARENA_ERR_ARG;
   arena_lock(a);
   bus_arena_result_t r = bus_arena_release_locked(a, lease_id, generation, consumer_slot);
   arena_unlock(a);
   return r;
}

static bus_arena_result_t bus_arena_cancel_locked(bus_arena_t *a, uint32_t lease_id)
{
   if (!a)
      return BUS_ARENA_ERR_ARG;
   bus_lease_t *l = lease_at(a, lease_id);
   if (!l)
      return BUS_ARENA_ERR_ARG;
   if (!l->producer_ref)
      return BUS_ARENA_ERR_STATE; /* nothing to cancel: already published */
   l->producer_ref = 0;
   lease_drop_if_zero(a, l);
   return BUS_ARENA_OK;
}

bus_arena_result_t bus_arena_cancel(bus_arena_t *a, uint32_t lease_id)
{
   if (!a)
      return BUS_ARENA_ERR_ARG;
   arena_lock(a);
   bus_arena_result_t r = bus_arena_cancel_locked(a, lease_id);
   arena_unlock(a);
   return r;
}

/* ---- reap ---- */

static void bus_arena_reap_producer_locked(bus_arena_t *a, uint32_t slot)
{
   if (!a || slot >= a->max_slots)
      return;
   for (uint32_t i = 0; i < a->lease_count; i++)
   {
      bus_lease_t *l = &a->leases[i];
      if (l->active && l->producer_ref && l->owner_slot == slot)
      {
         l->producer_ref = 0; /* orphaned-but-live: consumers still drain it */
         lease_drop_if_zero(a, l);
      }
   }
}

void bus_arena_reap_producer(bus_arena_t *a, uint32_t slot)
{
   if (!a)
      return;
   arena_lock(a);
   bus_arena_reap_producer_locked(a, slot);
   arena_unlock(a);
}

static void bus_arena_reap_consumer_locked(bus_arena_t *a, uint32_t slot)
{
   if (!a || slot >= a->max_slots)
      return;
   for (uint32_t i = 0; i < a->lease_count; i++)
   {
      bus_lease_t *l = &a->leases[i];
      if (l->active && bits_test(l->consumer_bits, slot))
      {
         bits_clear(l->consumer_bits, slot);
         lease_drop_if_zero(a, l);
      }
   }
}

void bus_arena_reap_consumer(bus_arena_t *a, uint32_t slot)
{
   if (!a)
      return;
   arena_lock(a);
   bus_arena_reap_consumer_locked(a, slot);
   arena_unlock(a);
}

/* ---- observers ---- */

uint32_t bus_arena_live_leases(const bus_arena_t *a, uint32_t slot)
{
   if (!a || slot >= a->max_slots)
      return 0;
   arena_lock(a);
   uint32_t n = a->live_per_slot[slot];
   arena_unlock(a);
   return n;
}

uint64_t bus_arena_bytes_in_use(const bus_arena_t *a)
{
   if (!a)
      return 0;
   arena_lock(a);
   uint64_t n = a->bytes_in_use;
   arena_unlock(a);
   return n;
}

uint32_t bus_arena_refcount(const bus_arena_t *a, uint32_t lease_id)
{
   if (!a)
      return 0;
   arena_lock(a);
   uint32_t n = (lease_id < a->lease_count && a->leases[lease_id].active)
                    ? lease_refcount(&a->leases[lease_id])
                    : 0;
   arena_unlock(a);
   return n;
}

const char *bus_arena_result_name(bus_arena_result_t r)
{
   switch (r)
   {
   case BUS_ARENA_OK:
      return "OK";
   case BUS_ARENA_ERR_ARG:
      return "ERR_ARG";
   case BUS_ARENA_ERR_NOSPACE:
      return "ERR_NOSPACE";
   case BUS_ARENA_ERR_NOLEASE:
      return "ERR_NOLEASE";
   case BUS_ARENA_ERR_CAP:
      return "ERR_CAP";
   case BUS_ARENA_ERR_STALE:
      return "ERR_STALE";
   case BUS_ARENA_ERR_NOTHOLDER:
      return "ERR_NOTHOLDER";
   case BUS_ARENA_ERR_STATE:
      return "ERR_STATE";
   default:
      return "ERR_UNKNOWN";
   }
}
