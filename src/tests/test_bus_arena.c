/* test_bus_arena.c: slice 4 of the event-bus feature tree.
 *
 * The lease allocator's whole reason to exist is that a slow or dead peer must
 * not corrupt a live reader, so the tests are the D3 lifecycle and its three
 * fault-injection cases, checked as outcomes:
 *
 *   - the refcount lifecycle: alloc takes the producer's ref, publish transfers
 *     it to the consumers, release drains it, and the region is reclaimed only
 *     at zero;
 *   - a stale generation is a typed error, never a read of reused bytes;
 *   - the per-client cap is refused synchronously at alloc, before any bytes are
 *     written and before any reap;
 *   - producer reap leaves a published region live for its readers;
 *   - consumer reap releases that consumer's refs so a dead consumer cannot
 *     strand the arena;
 *   - churn does not fragment the arena into uselessness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus_arena.h"

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      abort();
   }
}

static void must_result(bus_arena_result_t got, bus_arena_result_t want, const char *what)
{
   if (got != want)
   {
      fprintf(stderr, "FAIL: %s: expected %s, got %s\n", what, bus_arena_result_name(want),
              bus_arena_result_name(got));
      abort();
   }
}

static bus_arena_t *make(uint64_t size, uint32_t max_slots, uint32_t cap)
{
   static bus_arena_t a; /* big struct; keep it off the stack */
   static uint8_t *pool;
   free(pool);
   pool = malloc(size);
   must(pool != NULL, "pool alloc");
   must_result(bus_arena_init(&a, pool, size, max_slots, cap), BUS_ARENA_OK, "init");
   return &a;
}

/* ------------------------------------------------------------------ */
/* the refcount lifecycle                                              */

static void test_lifecycle(void)
{
   bus_arena_t *a = make(64 * 1024, 8, 16);

   uint32_t id;
   must_result(bus_arena_alloc(a, 1, 100, &id), BUS_ARENA_OK, "alloc");
   must(bus_arena_refcount(a, id) == 1, "alloc takes the producer's ref");
   must(bus_arena_live_leases(a, 1) == 1, "one live lease for the owner");

   /* Fill through the writable pointer, then publish to two observers. */
   uint8_t *w;
   must_result(bus_arena_fill_ptr(a, id, &w), BUS_ARENA_OK, "fill ptr");
   memset(w, 0xab, 100);

   bus_arena_ref_t ref;
   must_result(bus_arena_ref(a, id, &ref), BUS_ARENA_OK, "ref");

   uint8_t observers[2] = {2, 3};
   must_result(bus_arena_publish(a, id, observers, 2), BUS_ARENA_OK, "publish to 2");
   must(bus_arena_refcount(a, id) == 2, "publish transfers producer ref to 2 consumers");

   /* The producer's fill pointer is no longer valid after publish. */
   must_result(bus_arena_fill_ptr(a, id, &w), BUS_ARENA_ERR_STATE,
               "fill refused after publish");

   /* Each consumer reads in place, gated on the generation. */
   const uint8_t *r2, *r3;
   must_result(bus_arena_read_ptr(a, id, ref.generation, 2, &r2), BUS_ARENA_OK, "consumer 2 read");
   must_result(bus_arena_read_ptr(a, id, ref.generation, 3, &r3), BUS_ARENA_OK, "consumer 3 read");
   must(r2[0] == 0xab && r3[99] == 0xab, "consumers see the producer's bytes");

   /* A slot that was not an observer cannot read. */
   const uint8_t *r4;
   must_result(bus_arena_read_ptr(a, id, ref.generation, 4, &r4), BUS_ARENA_ERR_NOTHOLDER,
               "non-observer cannot read");

   must_result(bus_arena_release(a, id, ref.generation, 2), BUS_ARENA_OK, "consumer 2 releases");
   must(bus_arena_refcount(a, id) == 1, "one consumer left");
   must_result(bus_arena_release(a, id, ref.generation, 3), BUS_ARENA_OK, "consumer 3 releases");
   must(bus_arena_refcount(a, id) == 0, "region reclaimed at zero");
   must(bus_arena_live_leases(a, 1) == 0, "owner's live count back to zero");
   must(bus_arena_bytes_in_use(a) == 0, "arena empty");

   /* Publish to zero observers reclaims immediately — nobody can read it. */
   must_result(bus_arena_alloc(a, 1, 50, &id), BUS_ARENA_OK, "alloc again");
   must_result(bus_arena_publish(a, id, NULL, 0), BUS_ARENA_OK, "publish to nobody");
   must(bus_arena_refcount(a, id) == 0, "zero-observer publish reclaims");

   /* Cancel is the producer's unpublished error path. */
   must_result(bus_arena_alloc(a, 1, 50, &id), BUS_ARENA_OK, "alloc for cancel");
   must_result(bus_arena_cancel(a, id), BUS_ARENA_OK, "cancel");
   must(bus_arena_refcount(a, id) == 0, "cancel drops the producer ref");
   must(bus_arena_bytes_in_use(a) == 0, "cancelled lease freed");

   printf("  lifecycle: alloc->publish->release, zero-observer, cancel\n");
}

/* ------------------------------------------------------------------ */
/* stale generation                                                    */

static void test_stale_generation(void)
{
   bus_arena_t *a = make(64 * 1024, 8, 16);

   uint32_t id;
   must_result(bus_arena_alloc(a, 1, 64, &id), BUS_ARENA_OK, "alloc");
   bus_arena_ref_t ref;
   must_result(bus_arena_ref(a, id, &ref), BUS_ARENA_OK, "ref");
   uint8_t obs = 2;
   must_result(bus_arena_publish(a, id, &obs, 1), BUS_ARENA_OK, "publish");
   uint32_t old_gen = ref.generation;

   /* Consumer releases; the lease slot is now free and will be reused. */
   must_result(bus_arena_release(a, id, old_gen, 2), BUS_ARENA_OK, "release");

   /* Reuse the same table slot for a new lease — it gets a fresh generation. */
   uint32_t id2;
   must_result(bus_arena_alloc(a, 1, 64, &id2), BUS_ARENA_OK, "realloc");
   must(id2 == id, "same table slot reused");
   bus_arena_ref_t ref2;
   must_result(bus_arena_ref(a, id2, &ref2), BUS_ARENA_OK, "ref2");
   must(ref2.generation != old_gen, "reuse presents a fresh generation");

   /* A reader still holding the old generation must be refused, not handed the
    * new tenant's bytes. */
   uint8_t obs2 = 3;
   must_result(bus_arena_publish(a, id2, &obs2, 1), BUS_ARENA_OK, "publish2");
   const uint8_t *p;
   must_result(bus_arena_read_ptr(a, id2, old_gen, 3, &p), BUS_ARENA_ERR_STALE,
               "stale generation read is refused");
   must_result(bus_arena_release(a, id2, old_gen, 3), BUS_ARENA_ERR_STALE,
               "stale generation release is refused");

   printf("  stale generation: a reused lease refuses the old generation\n");
}

/* ------------------------------------------------------------------ */
/* per-client cap, synchronous at alloc                                */

static void test_cap_synchronous(void)
{
   const uint32_t cap = 3;
   bus_arena_t *a = make(64 * 1024, 8, cap);

   uint32_t ids[3];
   for (uint32_t i = 0; i < cap; i++)
      must_result(bus_arena_alloc(a, 5, 32, &ids[i]), BUS_ARENA_OK, "alloc up to cap");

   /* At the cap the very next alloc is refused immediately — no reap, no
    * heartbeat, before any byte is written. */
   uint32_t over;
   must_result(bus_arena_alloc(a, 5, 32, &over), BUS_ARENA_ERR_CAP,
               "alloc past the cap is refused synchronously");

   /* Another slot is unaffected — the cap is per client. */
   uint32_t other;
   must_result(bus_arena_alloc(a, 6, 32, &other), BUS_ARENA_OK, "a different slot is unaffected");

   /* Freeing one reopens a slot for the capped client. */
   must_result(bus_arena_cancel(a, ids[0]), BUS_ARENA_OK, "free one");
   must_result(bus_arena_alloc(a, 5, 32, &over), BUS_ARENA_OK, "capped client can alloc again");

   printf("  cap: refused synchronously at alloc, per client, reopens on free\n");
}

/* ------------------------------------------------------------------ */
/* fault injection                                                     */

/* (a) reap a producer while a consumer is mid-read: the region must stay live
 *     on intact bytes until the reader releases. */
static void test_reap_producer_with_reader(void)
{
   bus_arena_t *a = make(64 * 1024, 8, 16);

   uint32_t id;
   must_result(bus_arena_alloc(a, 1, 128, &id), BUS_ARENA_OK, "alloc");
   uint8_t *w;
   must_result(bus_arena_fill_ptr(a, id, &w), BUS_ARENA_OK, "fill");
   for (int i = 0; i < 128; i++)
      w[i] = (uint8_t)i;
   bus_arena_ref_t ref;
   must_result(bus_arena_ref(a, id, &ref), BUS_ARENA_OK, "ref");
   uint8_t obs = 2;
   must_result(bus_arena_publish(a, id, &obs, 1), BUS_ARENA_OK, "publish to consumer 2");

   /* The producer (slot 1) dies. Its ref is already gone after publish, but reap
    * must not disturb the consumer's live reference. */
   bus_arena_reap_producer(a, 1);
   must(bus_arena_refcount(a, id) == 1, "region stays live for the reader after producer reap");

   const uint8_t *p;
   must_result(bus_arena_read_ptr(a, id, ref.generation, 2, &p), BUS_ARENA_OK,
               "reader still reads after producer reap");
   for (int i = 0; i < 128; i++)
      must(p[i] == (uint8_t)i, "bytes intact after producer reap");

   must_result(bus_arena_release(a, id, ref.generation, 2), BUS_ARENA_OK, "reader releases");
   must(bus_arena_refcount(a, id) == 0, "reclaimed once the reader is done");

   /* Reap a producer that still holds an UNPUBLISHED lease: it is dropped. */
   must_result(bus_arena_alloc(a, 1, 64, &id), BUS_ARENA_OK, "alloc unpublished");
   bus_arena_reap_producer(a, 1);
   must(bus_arena_refcount(a, id) == 0, "unpublished lease dropped on producer reap");

   printf("  reap producer: a published region stays live for its reader\n");
}

/* (b) a consumer dies without releasing: its refs must be dropped so the arena
 *     is recovered, not stranded. */
static void test_reap_consumer_recovers(void)
{
   const uint64_t size = 8192;
   bus_arena_t *a = make(size, 8, 64);

   /* Fill the arena with leases all published to a consumer that then dies. */
   uint32_t ids[16];
   uint32_t n = 0;
   uint8_t obs = 2;
   for (; n < 16; n++)
   {
      if (bus_arena_alloc(a, 1, 256, &ids[n]) != BUS_ARENA_OK)
         break;
      must_result(bus_arena_publish(a, ids[n], &obs, 1), BUS_ARENA_OK, "publish to consumer 2");
   }
   must(n > 0, "some leases allocated");
   must(bus_arena_bytes_in_use(a) > 0, "arena in use");

   /* Consumer 2 dies without releasing anything. */
   bus_arena_reap_consumer(a, 2);
   must(bus_arena_bytes_in_use(a) == 0, "consumer reap recovers all the arena");
   for (uint32_t i = 0; i < n; i++)
      must(bus_arena_refcount(a, ids[i]) == 0, "every lease reclaimed");

   /* And the recovered arena is fully usable again — repeated consumer deaths do
    * not exhaust it. */
   uint32_t big;
   must_result(bus_arena_alloc(a, 1, 256, &big), BUS_ARENA_OK, "arena usable after reap");

   printf("  reap consumer: a dead consumer's refs are dropped, arena recovered\n");
}

/* ------------------------------------------------------------------ */
/* fragmentation                                                       */

static void test_no_fragmentation(void)
{
   const uint64_t size = 16384;
   bus_arena_t *a = make(size, 8, 4096);

   /* Alloc many small leases, free every other one, free the rest — coalescing
    * must return the arena to a single span so a near-full alloc succeeds. */
   uint32_t ids[32];
   uint32_t n = 0;
   for (; n < 32; n++)
      if (bus_arena_alloc(a, 1, 256, &ids[n]) != BUS_ARENA_OK)
         break;
   must(n >= 8, "several leases fit");

   for (uint32_t i = 0; i < n; i += 2)
      must_result(bus_arena_cancel(a, ids[i]), BUS_ARENA_OK, "free evens");
   for (uint32_t i = 1; i < n; i += 2)
      must_result(bus_arena_cancel(a, ids[i]), BUS_ARENA_OK, "free odds");

   must(bus_arena_bytes_in_use(a) == 0, "all freed");

   /* A single allocation of most of the arena must now succeed — if the free
    * list had not coalesced, it would be shattered into 256-byte holes. */
   uint32_t whole;
   must_result(bus_arena_alloc(a, 1, (uint32_t)(size - 256), &whole), BUS_ARENA_OK,
               "large alloc succeeds after churn (arena coalesced)");

   printf("  fragmentation: churn coalesces back to a usable whole\n");
}

/* Publishing to a list with a duplicate slot must be refused, and must leave the
 * lease untouched — the refcount cannot silently be one short of the claim. */
static void test_duplicate_observer_refused(void)
{
   bus_arena_t *a = make(64 * 1024, 8, 16);
   uint32_t id;
   must_result(bus_arena_alloc(a, 1, 64, &id), BUS_ARENA_OK, "alloc");
   uint8_t dup[3] = {2, 3, 2};
   must_result(bus_arena_publish(a, id, dup, 3), BUS_ARENA_ERR_ARG, "duplicate observer refused");
   /* The lease is untouched: still the producer's, publishable cleanly. */
   must(bus_arena_refcount(a, id) == 1, "refused publish left the producer ref");
   uint8_t ok[2] = {2, 3};
   must_result(bus_arena_publish(a, id, ok, 2), BUS_ARENA_OK, "clean publish afterwards");
   must(bus_arena_refcount(a, id) == 2, "refcount matches the two distinct observers");
   printf("  duplicate observer: refused, lease left intact\n");
}

/* Drive the lease table and the free list near their bounds with an adversarial
 * alternating free order, so a free-list overflow (a heap smash) would surface
 * under ASAN rather than only at 4096 leases in production. */
static void test_capacity_stress(void)
{
   const uint32_t slots = 128;
   /* Enough arena for many small leases; enough table entries to matter. */
   bus_arena_t *a = make(1u << 20, slots, BUS_ARENA_MAX_LEASES);

   uint32_t ids[2048];
   uint32_t n = 0;
   for (; n < 2048; n++)
      if (bus_arena_alloc(a, n % slots, 64, &ids[n]) != BUS_ARENA_OK)
         break;
   must(n >= 1024, "many leases allocated");

   /* Free every other one first — maximal fragmentation, most free spans. */
   for (uint32_t i = 0; i < n; i += 2)
      must_result(bus_arena_cancel(a, ids[i]), BUS_ARENA_OK, "free evens");
   /* Then the rest, which must coalesce everything back. */
   for (uint32_t i = 1; i < n; i += 2)
      must_result(bus_arena_cancel(a, ids[i]), BUS_ARENA_OK, "free odds");

   must(bus_arena_bytes_in_use(a) == 0, "all freed after adversarial churn");
   uint32_t whole;
   must_result(bus_arena_alloc(a, 0, (uint32_t)((1u << 20) - 64), &whole), BUS_ARENA_OK,
               "arena coalesced back to a single usable span");
   printf("  capacity stress: %u leases, alternating free, no overflow\n", n);
}

int main(void)
{
   printf("test_bus_arena:\n");
   test_lifecycle();
   test_stale_generation();
   test_cap_synchronous();
   test_duplicate_observer_refused();
   test_reap_producer_with_reader();
   test_reap_consumer_recovers();
   test_no_fragmentation();
   test_capacity_stress();
   printf("test_bus_arena: OK\n");
   return 0;
}
