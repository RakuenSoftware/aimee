/* test_bus_ring.c: slice 2 of the event-bus feature tree.
 *
 * The ring's correctness case is a concurrency argument, so the tests are
 * built to attack it rather than to demonstrate it:
 *
 *   - Geometry and attach validation, including headers that lie about their
 *     own size. In the real bus the header is written by another process, so
 *     "the header is wrong" is a case that has to be handled, not assumed away.
 *
 *   - Single-threaded wrap behaviour many times around the ring, which is
 *     where an index that aliased full with empty would show up.
 *
 *   - A threaded producer/consumer stress run carrying a checksummed sequence,
 *     so a lost, duplicated, reordered, or torn item is detected rather than
 *     merely made unlikely. Run under TSAN this also exercises the
 *     release/acquire pairs themselves.
 *
 * Every check uses must() rather than assert(), so -DNDEBUG cannot quietly
 * turn this into a test that passes because it stopped looking.
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
/* geometry                                                            */

static void test_geometry(void)
{
   must(bus_ring_bytes(64, 8) > 0, "valid geometry sizes");
   must(bus_ring_bytes(0, 8) == 0, "zero slot size rejected");
   must(bus_ring_bytes(64, 0) == 0, "zero capacity rejected");
   must(bus_ring_bytes(64, 3) == 0, "non-power-of-two capacity rejected");
   must(bus_ring_bytes(64, 1) == 0, "capacity below the minimum rejected");
   must(bus_ring_bytes(1u << 30, 1u << 20) == 0, "absurd geometry rejected");

   /* The header must leave the slots cache-line aligned, or the false sharing
    * the separated indices avoid comes straight back through the payload. */
   must(bus_ring_bytes(64, 8) % BUS_RING_CACHELINE == 0, "ring size is cache-line aligned");

   size_t need = bus_ring_bytes(64, 8);
   void *mem = calloc(1, need);
   must(mem != NULL, "alloc");
   bus_ring_t *r = NULL;

   must_result(bus_ring_init(mem, need - 1, 64, 8, &r), BUS_RING_ERR_MEM,
               "init into a short buffer");
   must_result(bus_ring_init(mem, need, 64, 7, &r), BUS_RING_ERR_GEOMETRY,
               "init with bad capacity");
   must_result(bus_ring_init(mem, need, 64, 8, &r), BUS_RING_OK, "init");
   must(bus_ring_capacity(r) == 8, "capacity readback");
   must(bus_ring_count(r) == 0, "fresh ring is empty");

   free(mem);
   printf("  geometry: bounds, alignment, and rejection\n");
}

/* bus_ring.c keeps header_bytes() private; the test only needs "smaller than
 * the header", and offsetof on the public type gives that without widening the
 * module's interface for a test's convenience. */
static size_t header_bytes_probe(void)
{
   return offsetof(bus_ring_t, slots) - 1;
}

/* A header written by another process is a claim, not a fact. These are the
 * lies that would otherwise turn into out-of-bounds access. */
static void test_attach_validation(void)
{
   size_t need = bus_ring_bytes(64, 8);
   void *mem = calloc(1, need);
   must(mem != NULL, "alloc");
   bus_ring_t *r = NULL;
   must_result(bus_ring_init(mem, need, 64, 8, &r), BUS_RING_OK, "init");

   bus_ring_t *got = NULL;
   must_result(bus_ring_attach(mem, need, &got), BUS_RING_OK, "attach to a good ring");

   uint32_t saved;

   saved = atomic_load_explicit(&r->magic, memory_order_relaxed);
   atomic_store_explicit(&r->magic, 0, memory_order_relaxed);
   must_result(bus_ring_attach(mem, need, &got), BUS_RING_ERR_MAGIC, "attach without magic");
   atomic_store_explicit(&r->magic, saved, memory_order_relaxed);

   saved = r->capacity;
   r->capacity = 7;
   must_result(bus_ring_attach(mem, need, &got), BUS_RING_ERR_GEOMETRY,
               "attach with non-power-of-two capacity");
   r->capacity = saved;

   saved = r->mask;
   r->mask = 0xffff;
   must_result(bus_ring_attach(mem, need, &got), BUS_RING_ERR_LAYOUT,
               "attach with a mask that disagrees with capacity");
   r->mask = saved;

   /* The important one: a header claiming a bigger ring than was mapped. */
   saved = r->capacity;
   r->capacity = 1024;
   r->mask = 1023;
   must_result(bus_ring_attach(mem, need, &got), BUS_RING_ERR_LAYOUT,
               "attach where the header outgrows the mapping");
   r->capacity = saved;
   r->mask = saved - 1;

   /* An index pair claiming more entries than exist must not become an
    * out-of-bounds slot index. */
   atomic_store_explicit(&r->head, 100, memory_order_relaxed);
   atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
   must_result(bus_ring_attach(mem, need, &got), BUS_RING_ERR_LAYOUT,
               "attach with an impossible index pair");
   atomic_store_explicit(&r->head, 0, memory_order_relaxed);

   must_result(bus_ring_attach(mem, header_bytes_probe(), &got), BUS_RING_ERR_MEM,
               "attach to a buffer smaller than the header");

   free(mem);
   printf("  attach: rejects five ways a header can lie\n");
}

/* ------------------------------------------------------------------ */
/* single-threaded behaviour                                           */

static void test_full_empty_and_wrap(void)
{
   const uint32_t cap = 4, slot = 32;
   size_t need = bus_ring_bytes(slot, cap);
   void *mem = calloc(1, need);
   bus_ring_t *r = NULL;
   must_result(bus_ring_init(mem, need, slot, cap, &r), BUS_RING_OK, "init");

   must(bus_ring_consume_begin(r) == NULL, "empty ring yields nothing");

   for (uint32_t i = 0; i < cap; i++)
   {
      void *s = bus_ring_produce_begin(r);
      must(s != NULL, "produce into a ring with room");
      memset(s, (int)i, slot);
      bus_ring_produce_commit(r);
   }
   must(bus_ring_count(r) == cap, "count at capacity");
   must(bus_ring_produce_begin(r) == NULL, "full ring refuses a producer");

   for (uint32_t i = 0; i < cap; i++)
   {
      const uint8_t *s = bus_ring_consume_begin(r);
      must(s != NULL, "consume from a non-empty ring");
      must(s[0] == (uint8_t)i, "FIFO order");
      bus_ring_consume_commit(r);
   }
   must(bus_ring_count(r) == 0, "count back to empty");
   must(bus_ring_consume_begin(r) == NULL, "drained ring yields nothing");

   /* Many laps, one item at a time, so every slot is reused repeatedly. An
    * index that wrapped rather than free-running would alias full with empty
    * somewhere in here. */
   for (uint32_t lap = 0; lap < 10000; lap++)
   {
      void *s = bus_ring_produce_begin(r);
      must(s != NULL, "produce during wrap");
      *(uint32_t *)s = lap;
      bus_ring_produce_commit(r);

      const void *c = bus_ring_consume_begin(r);
      must(c != NULL, "consume during wrap");
      must(*(const uint32_t *)c == lap, "value survives wrap");
      bus_ring_consume_commit(r);
   }
   must(bus_ring_count(r) == 0, "empty after 10000 laps");

   free(mem);
   printf("  full/empty and wrap: 10000 laps through a 4-slot ring\n");
}

/* ------------------------------------------------------------------ */
/* threaded stress                                                     */

#define STRESS_ITEMS   200000u
#define STRESS_SLOT    64u
#define STRESS_CAPACITY 64u

typedef struct
{
   uint64_t seq;
   uint64_t check; /* a function of seq, so a torn slot is visible */
   uint8_t pad[STRESS_SLOT - 2 * sizeof(uint64_t)];
} stress_item_t;

static uint64_t mix(uint64_t v)
{
   /* Any cheap avalanche will do; the point is that half a written item does
    * not accidentally satisfy the check. */
   v ^= v >> 33;
   v *= 0xff51afd7ed558ccdull;
   v ^= v >> 33;
   return v;
}

static bus_ring_t *g_ring;

static void *producer_thread(void *arg)
{
   (void)arg;
   for (uint64_t i = 0; i < STRESS_ITEMS; i++)
   {
      stress_item_t *s;
      /* Spin rather than sleep: a full ring is the interesting state, and
       * backing off would reduce how often the test visits it. */
      while ((s = (stress_item_t *)bus_ring_produce_begin(g_ring)) == NULL)
         ;
      s->seq = i;
      s->check = mix(i);
      bus_ring_produce_commit(g_ring);
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
      const stress_item_t *s;
      while ((s = (const stress_item_t *)bus_ring_consume_begin(g_ring)) == NULL)
         ;
      /* Order proves nothing was lost, duplicated, or reordered; the checksum
       * proves the slot was not read half-written. */
      must(s->seq == expect, "stress: sequence intact");
      must(s->check == mix(expect), "stress: item not torn");
      bus_ring_consume_commit(g_ring);
   }

   must(pthread_join(producer, NULL) == 0, "join producer");
   must(bus_ring_count(g_ring) == 0, "stress: ring drained");

   free(mem);
   printf("  threaded stress: %u items, checksummed, none lost or torn\n", STRESS_ITEMS);
}

int main(void)
{
   printf("test_bus_ring:\n");
   test_geometry();
   test_attach_validation();
   test_full_empty_and_wrap();
   test_threaded_stress();
   printf("test_bus_ring: OK\n");
   return 0;
}
