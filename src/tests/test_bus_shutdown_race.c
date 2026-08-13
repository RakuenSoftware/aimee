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
#include "db1/db1.h"
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

int main(void)
{
   printf("test_bus_shutdown_race:\n");

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busrace-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   assert(db1_init(":memory:") == 0);
   assert(server_obs_bus_configure() == 0);
   assert(obs_bus_start() == 0);

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

   /* Consistency: every guardrail event counted as WRITTEN must actually be in
    * db1 — no silent loss, no double count. */
   guardrail_event_counts_t counts;
   assert(db1_guardrail_event_counts_7d(&counts) == 0);
   int in_db = counts.block; /* all events are dry_run=0 final_action=block */
   printf("  %d producers; written=%llu dropped=%llu; db1 rows=%d\n", NTHREADS,
          (unsigned long long)written, (unsigned long long)dropped, in_db);
   if ((uint64_t)in_db != written)
   {
      fprintf(stderr,
              "FAIL: db1 has %d rows but %llu were counted written — a row was lost or "
              "double-counted across the shutdown race\n",
              in_db, (unsigned long long)written);
      return 1;
   }
   if (written == 0)
   {
      fprintf(stderr, "FAIL: no rows written — the race window was not exercised\n");
      return 1;
   }

   db1_shutdown();
   printf("test_bus_shutdown_race: OK (producers raced stop with no UAF and no lost/double row)\n");
   return 0;
}
