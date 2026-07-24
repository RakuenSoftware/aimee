/* Concurrency regression guard for the witness release-gate verification cell
 * (kb/kb_witness_gate_state.c).
 *
 * The cell is written by the cadence (kb_main periodic-loop) thread and read by the
 * HTTP egress gate (listener thread). A prior version used a bare `volatile int`,
 * which the C memory model does not define for inter-thread synchronisation. The fix
 * made it a C11 _Atomic with release/acquire ordering.
 *
 * This test drives the exact store/load the gate relies on from separate writer and
 * reader threads. Two things it asserts on every build:
 *   - liveness: the reader observes writes progress (it sees more than one distinct
 *     published value), so the hand-off is not silently wedged;
 *   - domain safety: the reader NEVER observes a value outside {-1,0,1}. A torn or
 *     otherwise undefined read of the cell is the only way an out-of-domain value
 *     could appear, so any such observation fails the test.
 *
 * Its real teeth are under ThreadSanitizer (scripts/run-witness-gate-tsan.sh): if the
 * cell is ever reverted to a non-atomic type / access, TSan reports a data race
 * between kb_witness_gate_state_set and kb_witness_gate_state_get and the run fails.
 * Under a normal build it still catches a wedged or domain-violating hand-off. */

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

#include "kb/kb_witness_gate_state.h"

/* A wide window so TSan can catch an unsynchronised access. */
#define MIN_ITERS 2000000
/* Transitions the reader must observe before the writer is allowed to stop —
 * the liveness guarantee that removes the scheduling flake below. */
#define NEED_DISTINCT 8
/* Safety cap far above MIN_ITERS: a genuinely wedged hand-off (reader never sees
 * the value change) fails as a bounded error instead of hanging. */
#define HARD_CAP 200000000L

static _Atomic int stop;
static _Atomic long domain_violations;
static _Atomic long distinct_seen; /* reads that differed from the prior read (published live) */

static void *writer(void *unused)
{
   (void)unused;
   /* Cycle through the full valid domain so the reader has all three states to
    * observe, in a non-trivial order (mirrors the cadence: unknown at boot, then
    * flips between clean and not-clean). */
   static const int seq[] = {-1, 1, 0, 1, 0, 0, 1, -1};
   /* Do not stop after a fixed count: on a loaded runner (unit-tests runs the
    * suite with xargs -P) the reader can be starved for the whole loop and see a
    * single constant value — the old "no progress" flake. Yield periodically so
    * the reader is scheduled, and keep publishing until it has actually observed
    * NEED_DISTINCT transitions AND the TSan window is wide enough. HARD_CAP bounds
    * a truly broken hand-off. */
   for (long i = 0;; i++)
   {
      kb_witness_gate_state_set(seq[i & 7]);
      if ((i & 0x3FF) == 0)
         sched_yield();
      if (i >= MIN_ITERS &&
          atomic_load_explicit(&distinct_seen, memory_order_relaxed) >= NEED_DISTINCT)
         break;
      if (i >= HARD_CAP)
         break;
   }
   atomic_store_explicit(&stop, 1, memory_order_release);
   return NULL;
}

static void *reader(void *unused)
{
   (void)unused;
   int prev = kb_witness_gate_state_get();
   long local_bad = 0;
   while (!atomic_load_explicit(&stop, memory_order_acquire))
   {
      int v = kb_witness_gate_state_get();
      if (v != -1 && v != 0 && v != 1)
         local_bad++;
      if (v != prev)
      {
         /* Publish each transition live so the writer can see the reader is making
          * progress and stop only then. */
         atomic_fetch_add_explicit(&distinct_seen, 1, memory_order_relaxed);
         prev = v;
      }
   }
   atomic_fetch_add_explicit(&domain_violations, local_bad, memory_order_relaxed);
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
   /* Liveness: the reader must have observed the published value change. If it saw a
    * single constant value the whole time the hand-off is not actually working. */
   if (distinct < 2)
   {
      fprintf(stderr, "witness gate race: FAIL — reader saw no progress (distinct=%ld)\n",
              distinct);
      return 1;
   }
   printf("witness gate race: ok (distinct transitions observed=%ld, out-of-domain=0)\n", distinct);
   return 0;
}
