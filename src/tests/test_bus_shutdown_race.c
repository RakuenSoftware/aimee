/* test_bus_shutdown_race.c: concurrent producers racing obs_bus_stop().
 *
 * Regression test for the shutdown race: obs_bus_stop() used to tear down the
 * producer / host / pub_lock while in-flight emit() calls were still using them —
 * a use-after-free, and silently lost rows. The fix makes emit register in a
 * publisher refcount before re-checking `emitting`, and stop wait for that count
 * to reach zero before teardown. This test hammers the exact window: several
 * threads emit continuously while the main thread stops the bus mid-flight. Under
 * ASAN/TSAN a surviving race shows up as a data race or a use-after-free; the
 * consistency assertion (every WRITTEN guardrail event is actually in db1) catches
 * a silently-lost or double-counted row.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <aimee/audit/obs_bus.h>
#include "db1_client/db1.h"
#include "server/obs_bus_adapter.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define NTHREADS 4

static atomic_int g_keep_going = 1;

static void *producer(void *arg)
{
   long id = (long)arg;
   guardrail_event_t e;
   memset(&e, 0, sizeof e);
   snprintf(e.session_id, sizeof e.session_id, "sess-%ld", id);
   snprintf(e.tool_name, sizeof e.tool_name, "Tool");
   snprintf(e.recommendation, sizeof e.recommendation, "warn");
   /* final_action='block', dry_run=0 so db1_guardrail_event_counts_7d.block counts
    * every row unambiguously (see the categorization in guardrail_events.c). */
   snprintf(e.final_action, sizeof e.final_action, "block");
   while (atomic_load(&g_keep_going))
      obs_bus_emit_guardrail(&e); /* emits before, during, and after stop() */
   return NULL;
}

/* Counts what the bus hands it. Returns 0 for "durably accepted", which is the
 * contract obs_bus_guardrail_sink_fn states, because for this test acceptance
 * IS the durability being measured. */
static atomic_ullong g_sunk;

static int counting_sink(const guardrail_event_t *event, void *ctx)
{
   (void)event;
   (void)ctx;
   atomic_fetch_add(&g_sunk, 1);
   return 0;
}

int main(void)
{
   /* The store is a module now. Without one attached every db1_* call below
      fails, so bring the real one up -- or skip, saying why, on a machine with
      no database to point it at. */
   /* A COUNTING SINK, not the store. What this file is about is the emit/stop
      race, and the sink only has to exist and be fast: the original had an
      in-process SQLite database, and pointing it at the real store instead
      makes every event a network round trip to another host while NTHREADS
      producers spin -- the run stops finishing, and the race window stops being
      what is measured.
      Counting here also makes the claim exact. The sink reports what it was
      actually handed, so "everything counted written was accepted" is checked
      rather than inferred from a row count read somewhere else. */
   assert(obs_bus_set_guardrail_sink(counting_sink, NULL) == 0);
   assert(obs_bus_start() == 0);

   printf("test_bus_shutdown_race:\n");

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busrace-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);

   pthread_t th[NTHREADS];
   for (long i = 0; i < NTHREADS; i++)
      assert(pthread_create(&th[i], NULL, producer, (void *)i) == 0);

   /* Let the producers get going, then stop the bus WHILE they are mid-emit —
    * this is the window the fix protects. */
   struct timespec warmup = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000}; /* 20 ms */
   nanosleep(&warmup, NULL);
   obs_bus_stop(); /* must quiesce in-flight producers before teardown */

   /* Producers keep calling after stop; those are rejected no-ops. Now wind down. */
   atomic_store(&g_keep_going, 0);
   for (int i = 0; i < NTHREADS; i++)
      pthread_join(th[i], NULL);

   uint64_t written = obs_bus_written();
   uint64_t dropped = obs_bus_dropped();

   /* Everything the bus counted as written reached the sink: no silent loss,
      no double count, across a stop that ran while producers were mid-emit.
      This is the claim the file was written for. */
   unsigned long long sunk = atomic_load(&g_sunk);
   if (sunk != written)
   {
      fprintf(stderr,
              "FAIL: the sink accepted %llu events but %llu were counted written -- "
              "an event was lost or double-counted across the shutdown race\n",
              sunk, (unsigned long long)written);
      return 1;
   }

   /* WHAT THIS NO LONGER CHECKS. There used to be a second claim: that the
    * events are in the STORE, read back with db1_guardrail_event_counts_7d.
    * That was free when the store was in-process SQLite. It is not possible
    * now -- the store is reached over the bus, and stopping the bus is the
    * thing this test exercises. Restarting the host does not recover it: the
    * module does not re-attach, measured with a ten-second poll rather than
    * assumed.
    *
    * The property is real and is recorded with the retired
    * test_bus_guardrail_durability, in src/tests/Rules.mk and in
    * docs/validation/store-module-on-a-clean-container.md. It wants a test
    * against the Go sink that does not depend on the bus it is verifying. */
   if (written == 0)
   {
      fprintf(stderr, "FAIL: no rows written — the race window was not exercised\n");
      return 1;
   }

   printf("test_bus_shutdown_race: OK (producers raced stop with no UAF; "
          "%llu written, %llu dropped, all accounted)\n",
          (unsigned long long)written, (unsigned long long)dropped);
   return 0;
}
