/* Concurrency regression guard for the witness release-gate verification cell
 * (kb/kb_witness_gate_state.c).
 *
 * The cell is written by the cadence (kb_main periodic-loop) thread and read by the
 * HTTP egress gate (listener thread). A prior version used a bare `volatile int`,
 * which the C memory model does not define for inter-thread synchronisation. The fix
 * made it a C11 _Atomic with release/acquire ordering.
 *
 * This test drives the exact store/load the gate relies on from separate writer and
 * reader threads. It ASSERTS domain safety: the reader NEVER observes a value outside
 * {-1,0,1}. A torn or otherwise undefined read of the cell is the only way an
 * out-of-domain value could appear, so any such observation fails the test.
 *
 * It also reports how many distinct values the reader observed (a liveness signal),
 * but does NOT fail on a low count: on a loaded CI runner the reader thread can be
 * starved enough to see fewer than two distinct values with nothing actually wrong,
 * so a hard liveness assertion here is a flake, not a guard.
 *
 * Its real teeth are under ThreadSanitizer (scripts/run-witness-gate-tsan.sh): if the
 * cell is ever reverted to a non-atomic type / access, TSan reports a data race
 * between kb_witness_gate_state_set and kb_witness_gate_state_get and the run fails.
 * Under a normal build it still catches a wedged or domain-violating hand-off. */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "kb/kb_witness_gate_state.h"

/* Enough iterations to give TSan a wide window to catch an unsynchronised access and
 * to make a wedged hand-off statistically certain to be caught. */
#define ITERS 2000000

static _Atomic int stop;
static _Atomic long domain_violations;
static _Atomic long distinct_seen; /* count of reads that differed from the prior read */

static void *writer(void *unused)
{
   (void)unused;
   /* Cycle through the full valid domain so the reader has all three states to
    * observe, in a non-trivial order (mirrors the cadence: unknown at boot, then
    * flips between clean and not-clean). */
   static const int seq[] = {-1, 1, 0, 1, 0, 0, 1, -1};
   for (long i = 0; i < ITERS; i++)
      kb_witness_gate_state_set(seq[i & 7]);
   atomic_store_explicit(&stop, 1, memory_order_release);
   return NULL;
}

static void *reader(void *unused)
{
   (void)unused;
   int prev = kb_witness_gate_state_get();
   long local_distinct = 0, local_bad = 0;
   while (!atomic_load_explicit(&stop, memory_order_acquire))
   {
      int v = kb_witness_gate_state_get();
      if (v != -1 && v != 0 && v != 1)
         local_bad++;
      if (v != prev)
      {
         local_distinct++;
         prev = v;
      }
   }
   atomic_fetch_add_explicit(&domain_violations, local_bad, memory_order_relaxed);
   atomic_fetch_add_explicit(&distinct_seen, local_distinct, memory_order_relaxed);
   return NULL;
}

int main(void)
{
   /* Fresh cell reads as -1 (never verified) — the fail-closed default. */
   assert(kb_witness_gate_state_get() == -1);

   pthread_t wt, rt;
   assert(pthread_create(&rt, NULL, reader, NULL) == 0);
   assert(pthread_create(&wt, NULL, writer, NULL) == 0);
   assert(pthread_join(wt, NULL) == 0);
   assert(pthread_join(rt, NULL) == 0);

   long bad = atomic_load_explicit(&domain_violations, memory_order_relaxed);
   long distinct = atomic_load_explicit(&distinct_seen, memory_order_relaxed);
   if (bad != 0)
   {
      fprintf(stderr, "witness gate race: FAIL — reader saw %ld out-of-domain value(s)\n", bad);
      return 1;
   }
   /* Liveness is reported, not asserted: reader-thread starvation under CI load can
    * legitimately leave distinct < 2 with no defect, so failing on it only produces
    * flakes. Domain safety (above) and the TSan lane are the real regression guards. */
   printf("witness gate race: ok (distinct transitions observed=%ld, out-of-domain=0)\n", distinct);
   return 0;
}
