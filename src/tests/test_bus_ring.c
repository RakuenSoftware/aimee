/* test_bus_ring.c: slice 2 of the event-bus feature tree.
 *
 * The ring's correctness case is a concurrency argument, so the tests are built
 * to attack it rather than to demonstrate it:
 *
 *   - Geometry and attach validation, including headers that lie about their
 *     own size. In the real bus the header is written by another process, so
 *     "the header is wrong" is a case to handle, not to assume away.
 *
 *   - The handle boundary: after attach, mutating the shared header must not
 *     change where this process addresses. That is the property that stops a
 *     hostile peer steering our writes out of the mapping.
 *
 *   - Cache-line separation asserted by offset, not by total size. A test that
 *     only checked the ring's size would pass even with head sharing a line
 *     with the metadata.
 *
 *   - Wrap behaviour many laps around the ring, where an index that aliased
 *     full with empty would show up, and attach against a ring already in use.
 *
 *   - A threaded stress run whose every payload byte is a function of the
 *     sequence number, so a lost, duplicated, reordered, or partially-written
 *     item is detected rather than merely made unlikely.
 *
 * Every check uses must() rather than assert(), so -DNDEBUG cannot quietly turn
 * this into a test that passes because it stopped looking.
 */
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus_ring.h"

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

static void must_result(bus_ring_result_t got, bus_ring_result_t want, const char *what)
{
   if (got != want)
   {
      fprintf(stderr, "FAIL: %s: expected %s, got %s\n", what, bus_ring_result_name(want),
              bus_ring_result_name(got));
      abort();
   }
}

/* ------------------------------------------------------------------ */
/* geometry and layout                                                 */

static void test_geometry(void)
{
   must(bus_ring_bytes(64, 8) > 0, "valid geometry sizes");
   must(bus_ring_bytes(0, 8) == 0, "zero slot size rejected");
   must(bus_ring_bytes(64, 0) == 0, "zero capacity rejected");
   must(bus_ring_bytes(64, 3) == 0, "non-power-of-two capacity rejected");
   must(bus_ring_bytes(64, 1) == 0, "capacity below the minimum rejected");
   must(bus_ring_bytes(1u << 30, 1u << 20) == 0, "absurd geometry rejected");

   /* Asserted by offset. The old form — total size divisible by the cache line
    * — was satisfiable with head at offset 32, so the false-sharing guarantee
    * it claimed to check was never actually checked. */
   must(offsetof(bus_ring_shared_t, head) % BUS_RING_CACHELINE == 0, "head begins a cache line");
   must(offsetof(bus_ring_shared_t, head) >= BUS_RING_CACHELINE,
        "head does not share the metadata line");
   must(offsetof(bus_ring_shared_t, tail) >= offsetof(bus_ring_shared_t, head) + BUS_RING_CACHELINE,
        "tail does not share head's line");
   must(offsetof(bus_ring_shared_t, slots) >=
            offsetof(bus_ring_shared_t, tail) + BUS_RING_CACHELINE,
        "slots do not share tail's line");

   size_t need = bus_ring_bytes(64, 8);
   void *mem = calloc(1, need);
   must(mem != NULL, "alloc");
   bus_ring_t r;

   must_result(bus_ring_init(mem, need - 1, 64, 8, &r), BUS_RING_ERR_MEM,
               "init into a short buffer");
   must_result(bus_ring_init(mem, need, 64, 7, &r), BUS_RING_ERR_GEOMETRY,
               "init with bad capacity");
   must_result(bus_ring_init(mem, need, 64, 8, &r), BUS_RING_OK, "init");
   must(bus_ring_capacity(&r) == 8, "capacity readback");
   must(bus_ring_count(&r) == 0, "fresh ring is empty");

   free(mem);
   printf("  geometry: bounds, cache-line offsets, and rejection\n");
}

/* A header written by another process is a claim, not a fact. These are the
 * lies that would otherwise turn into out-of-bounds access. */
static void test_attach_validation(void)
{
   size_t need = bus_ring_bytes(64, 8);
   void *mem = calloc(1, need);
   must(mem != NULL, "alloc");
   bus_ring_t r;
   must_result(bus_ring_init(mem, need, 64, 8, &r), BUS_RING_OK, "init");
   bus_ring_shared_t *s = r.shared;

   bus_ring_t got;
   must_result(bus_ring_attach(mem, need, &got), BUS_RING_OK, "attach to a good ring");

   struct
   {
      const char *what;
      _Atomic uint32_t *field;
      uint32_t bad;
   } lies[] = {
       {"no magic", &s->magic, 0},
       {"capacity not a power of two", &s->capacity, 7},
       {"capacity above the maximum", &s->capacity, 1u << 21},
       {"capacity outgrows the mapping", &s->capacity, 1024},
       {"slot size zero", &s->slot_size, 0},
       {"slot size above the maximum", &s->slot_size, 1u << 25},
       {"mask disagrees with capacity", &s->mask, 0xffff},
       {"slots offset moved", &s->slots_off, 8},
   };

   for (size_t i = 0; i < sizeof lies / sizeof lies[0]; i++)
   {
      uint32_t saved = atomic_load_explicit(lies[i].field, memory_order_relaxed);
      atomic_store_explicit(lies[i].field, lies[i].bad, memory_order_relaxed);
      if (bus_ring_attach(mem, need, &got) == BUS_RING_OK)
      {
         fprintf(stderr, "FAIL: attach accepted a header that lied: %s\n", lies[i].what);
         abort();
      }
      atomic_store_explicit(lies[i].field, saved, memory_order_relaxed);
   }

   /* An index pair claiming more entries than exist must not become an
    * out-of-bounds slot index. */
   atomic_store_explicit(&s->head, 100, memory_order_relaxed);
   atomic_store_explicit(&s->tail, 0, memory_order_relaxed);
   must_result(bus_ring_attach(mem, need, &got), BUS_RING_ERR_LAYOUT,
               "attach with an impossible index pair");

   /* But exactly-full is ordinary operation, not corruption. */
   atomic_store_explicit(&s->head, 8, memory_order_relaxed);
   atomic_store_explicit(&s->tail, 0, memory_order_relaxed);
   must_result(bus_ring_attach(mem, need, &got), BUS_RING_OK, "attach to an exactly-full ring");
   atomic_store_explicit(&s->head, 0, memory_order_relaxed);

   must_result(bus_ring_attach(mem, offsetof(bus_ring_shared_t, slots) - 1, &got), BUS_RING_ERR_MEM,
               "attach to a buffer smaller than the header");

   free(mem);
   printf("  attach: rejects %zu ways a header can lie, accepts exactly-full\n",
          sizeof lies / sizeof lies[0]);
}

/* The handle is the safety boundary: once attached, nothing a peer writes to
 * the shared header may change where this process addresses. */
static void test_handle_isolates_geometry(void)
{
   const uint32_t cap = 8, slot = 64;
   size_t need = bus_ring_bytes(slot, cap);
   void *mem = calloc(1, need);
   bus_ring_t r;
   must_result(bus_ring_init(mem, need, slot, cap, &r), BUS_RING_OK, "init");

   void *first = bus_ring_produce_begin(&r);
   must(first != NULL, "produce into a fresh ring");

   /* A hostile peer rewrites the geometry to values that, if the hot path
    * re-read them, would address far outside the mapping. */
   atomic_store_explicit(&r.shared->slot_size, 1u << 24, memory_order_relaxed);
   atomic_store_explicit(&r.shared->capacity, 1u << 20, memory_order_relaxed);
   atomic_store_explicit(&r.shared->mask, (1u << 20) - 1, memory_order_relaxed);
   atomic_store_explicit(&r.shared->slots_off, 0xfffff, memory_order_relaxed);

   must(bus_ring_produce_begin(&r) == first,
        "the hot path still addresses the same slot after the header is scribbled on");
   must(bus_ring_capacity(&r) == cap, "handle capacity is unaffected");

   uint8_t *p = (uint8_t *)bus_ring_produce_begin(&r);
   must(p >= (uint8_t *)mem && p + slot <= (uint8_t *)mem + need,
        "the slot remains inside the mapping");

   free(mem);
   printf("  handle: copied geometry, so a scribbled header cannot move addressing\n");
}

/* ------------------------------------------------------------------ */
/* single-threaded behaviour                                           */

static void test_full_empty_and_wrap(void)
{
   const uint32_t cap = 4, slot = 32;
   size_t need = bus_ring_bytes(slot, cap);
   void *mem = calloc(1, need);
   bus_ring_t r;
   must_result(bus_ring_init(mem, need, slot, cap, &r), BUS_RING_OK, "init");

   must(bus_ring_consume_begin(&r) == NULL, "empty ring yields nothing");

   for (uint32_t i = 0; i < cap; i++)
   {
      void *s = bus_ring_produce_begin(&r);
      must(s != NULL, "produce into a ring with room");
      memset(s, (int)i, slot);
      bus_ring_produce_commit(&r);
   }
   must(bus_ring_count(&r) == cap, "count at capacity");
   must(bus_ring_produce_begin(&r) == NULL, "full ring refuses a producer");

   for (uint32_t i = 0; i < cap; i++)
   {
      const uint8_t *s = bus_ring_consume_begin(&r);
      must(s != NULL, "consume from a non-empty ring");
      must(s[0] == (uint8_t)i, "FIFO order");
      bus_ring_consume_commit(&r);
   }
   must(bus_ring_count(&r) == 0, "count back to empty");
   must(bus_ring_consume_begin(&r) == NULL, "drained ring yields nothing");

   /* Many laps, one item at a time, so every slot is reused repeatedly. An
    * index that wrapped rather than free-running would alias full with empty
    * somewhere in here. */
   for (uint32_t lap = 0; lap < 10000; lap++)
   {
      void *s = bus_ring_produce_begin(&r);
      must(s != NULL, "produce during wrap");
      *(uint32_t *)s = lap;
      bus_ring_produce_commit(&r);

      const void *c = bus_ring_consume_begin(&r);
      must(c != NULL, "consume during wrap");
      must(*(const uint32_t *)c == lap, "value survives wrap");
      bus_ring_consume_commit(&r);
   }
   must(bus_ring_count(&r) == 0, "empty after 10000 laps");

   /* A long-lived ring with advanced counters is exactly what a host adopts,
    * so attach must work on one — not only on a freshly-initialised ring. */
   for (uint32_t i = 0; i < 3; i++)
   {
      void *s = bus_ring_produce_begin(&r);
      must(s != NULL, "seed residual items");
      *(uint32_t *)s = 0xabcd0000u + i;
      bus_ring_produce_commit(&r);
   }
   bus_ring_t re;
   must_result(bus_ring_attach(mem, need, &re), BUS_RING_OK, "attach to a used ring");
   must(bus_ring_count(&re) == 3, "residual count survives attach");
   for (uint32_t i = 0; i < 3; i++)
   {
      const void *c = bus_ring_consume_begin(&re);
      must(c != NULL, "consume residual through the re-attached handle");
      must(*(const uint32_t *)c == 0xabcd0000u + i, "residual value intact");
      bus_ring_consume_commit(&re);
   }

   free(mem);
   printf("  wrap: 10000 laps, then attach to the used ring and drain it\n");
}

/* ------------------------------------------------------------------ */
/* threaded stress                                                     */

#define STRESS_ITEMS    200000u
#define STRESS_SLOT     64u
#define STRESS_CAPACITY 64u

static uint64_t mix(uint64_t v)
{
   v ^= v >> 33;
   v *= 0xff51afd7ed558ccdull;
   v ^= v >> 33;
   return v;
}

/* Every byte of the slot is a function of the sequence number, so a torn write
 * anywhere in the item is caught — not just in the two words a header-only
 * checksum would have covered. */
static void fill_item(uint8_t *p, uint64_t seq)
{
   for (uint32_t i = 0; i < STRESS_SLOT; i += 8)
   {
      uint64_t word = mix(seq * 131 + i);
      memcpy(p + i, &word, sizeof word);
   }
   memcpy(p, &seq, sizeof seq); /* first word carries the sequence itself */
}

static int item_matches(const uint8_t *p, uint64_t seq)
{
   uint8_t expect[STRESS_SLOT];
   fill_item(expect, seq);
   return memcmp(p, expect, STRESS_SLOT) == 0;
}

static bus_ring_t g_ring;

/* Bounded, because an unbounded spin turns a dead peer into a CI hang that is
 * indistinguishable from a slow machine. */
#define SPIN_LIMIT 2000000000ull

static void *producer_thread(void *arg)
{
   (void)arg;
   for (uint64_t i = 0; i < STRESS_ITEMS; i++)
   {
      uint8_t *s;
      uint64_t spins = 0;
      while ((s = (uint8_t *)bus_ring_produce_begin(&g_ring)) == NULL)
         if (++spins > SPIN_LIMIT)
         {
            fprintf(stderr, "FAIL: producer spun out waiting for room at item %llu\n",
                    (unsigned long long)i);
            abort();
         }
      fill_item(s, i);
      bus_ring_produce_commit(&g_ring);
   }
   return NULL;
}

static void test_threaded_stress(void)
{
   size_t need = bus_ring_bytes(STRESS_SLOT, STRESS_CAPACITY);
   void *mem = calloc(1, need);
   must(mem != NULL, "alloc");
   must_result(bus_ring_init(mem, need, STRESS_SLOT, STRESS_CAPACITY, &g_ring), BUS_RING_OK,
               "init");

   pthread_t producer;
   must(pthread_create(&producer, NULL, producer_thread, NULL) == 0, "spawn producer");

   for (uint64_t expect = 0; expect < STRESS_ITEMS; expect++)
   {
      const uint8_t *s;
      uint64_t spins = 0;
      while ((s = (const uint8_t *)bus_ring_consume_begin(&g_ring)) == NULL)
         if (++spins > SPIN_LIMIT)
         {
            fprintf(stderr, "FAIL: consumer spun out waiting for item %llu\n",
                    (unsigned long long)expect);
            abort();
         }
      /* Order proves nothing was lost, duplicated, or reordered; the full-slot
       * comparison proves no byte of it was read half-written. */
      must(item_matches(s, expect), "stress: whole item intact and in sequence");
      bus_ring_consume_commit(&g_ring);
   }

   must(pthread_join(producer, NULL) == 0, "join producer");
   must(bus_ring_count(&g_ring) == 0, "stress: ring drained");

   free(mem);
   printf("  threaded stress: %u items, every byte verified, none lost or torn\n", STRESS_ITEMS);
}

int main(void)
{
   printf("test_bus_ring:\n");
   test_geometry();
   test_attach_validation();
   test_handle_isolates_geometry();
   test_full_empty_and_wrap();
   test_threaded_stress();
   printf("test_bus_ring: OK\n");
   return 0;
}
