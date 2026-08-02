#include "dependency_breaker.h"

#include <assert.h>
#include <stdio.h>

static dependency_breaker_t g_breaker = DEPENDENCY_BREAKER_INITIALIZER;

int main(void)
{
   const int64_t started = 100000;
   int64_t retry = -1;
   dependency_breaker_snapshot_t snap;

   assert(dependency_breaker_allow(&g_breaker, started, &retry) == 1);
   for (unsigned i = 0; i < DEPENDENCY_BREAKER_DEFAULT_THRESHOLD; i++)
      dependency_breaker_report_failure(&g_breaker, started, DEPENDENCY_BREAKER_DEFAULT_THRESHOLD,
                                        DEPENDENCY_BREAKER_DEFAULT_BASE_MS,
                                        DEPENDENCY_BREAKER_DEFAULT_MAX_MS);

   assert(dependency_breaker_allow(&g_breaker, started, &retry) == 0);
   assert(retry >= DEPENDENCY_BREAKER_DEFAULT_BASE_MS);
   assert(retry <= DEPENDENCY_BREAKER_DEFAULT_BASE_MS * 5 / 4);
   dependency_breaker_snapshot(&g_breaker, started, &snap);
   assert(snap.open == 1 && snap.failure_streak == 3 && snap.suppressed_calls == 1);

   int64_t first_retry_at = started + retry;
   assert(dependency_breaker_allow(&g_breaker, first_retry_at, NULL) == 1);
   assert(dependency_breaker_allow(&g_breaker, first_retry_at, NULL) == 0);
   dependency_breaker_report_failure(
       &g_breaker, first_retry_at, DEPENDENCY_BREAKER_DEFAULT_THRESHOLD,
       DEPENDENCY_BREAKER_DEFAULT_BASE_MS, DEPENDENCY_BREAKER_DEFAULT_MAX_MS);
   dependency_breaker_snapshot(&g_breaker, first_retry_at, &snap);
   assert(snap.retry_after_ms >= DEPENDENCY_BREAKER_DEFAULT_BASE_MS * 2);
   assert(snap.retry_after_ms <= DEPENDENCY_BREAKER_DEFAULT_BASE_MS * 5 / 2);

   int64_t second_retry_at = first_retry_at + snap.retry_after_ms;
   assert(dependency_breaker_allow(&g_breaker, second_retry_at, NULL) == 1);
   dependency_breaker_report_success(&g_breaker, second_retry_at);
   assert(dependency_breaker_allow(&g_breaker, second_retry_at, NULL) == 1);
   dependency_breaker_snapshot(&g_breaker, second_retry_at, &snap);
   assert(snap.open == 0 && snap.failure_streak == 0 && snap.last_success_ms == second_retry_at);

   dependency_breaker_reset(&g_breaker);
   dependency_breaker_snapshot(&g_breaker, started, &snap);
   assert(snap.suppressed_calls == 0 && snap.last_failure_ms == 0);

   /* The KB transport logs an outage by comparing the snapshot either side of a
    * report, so "did this failure open the breaker?" must be answerable exactly
    * once per outage. If the open edge repeated, a sustained outage would log
    * per request; if it never appeared, the outage stayed silent — which is how
    * an unreachable KB previously looked identical to an empty index. */
   dependency_breaker_snapshot_t before, after;
   unsigned open_edges = 0, close_edges = 0;
   const int64_t t0 = 500000;
   for (unsigned i = 0; i < DEPENDENCY_BREAKER_DEFAULT_THRESHOLD * 3U; i++)
   {
      dependency_breaker_snapshot(&g_breaker, t0, &before);
      dependency_breaker_report_failure(&g_breaker, t0, DEPENDENCY_BREAKER_DEFAULT_THRESHOLD,
                                        DEPENDENCY_BREAKER_DEFAULT_BASE_MS,
                                        DEPENDENCY_BREAKER_DEFAULT_MAX_MS);
      dependency_breaker_snapshot(&g_breaker, t0, &after);
      if (after.open && !before.open)
         open_edges++;
   }
   assert(open_edges == 1);

   dependency_breaker_snapshot(&g_breaker, t0, &before);
   dependency_breaker_report_success(&g_breaker, t0);
   dependency_breaker_snapshot(&g_breaker, t0, &after);
   if (before.open && !after.open)
      close_edges++;
   assert(close_edges == 1);
   assert(before.open_count > 0); /* the log line reports how many intervals elapsed */

   /* A success while already closed is not a recovery and must not log one. */
   dependency_breaker_snapshot(&g_breaker, t0, &before);
   dependency_breaker_report_success(&g_breaker, t0);
   assert(before.open == 0);

   /* A denied call must hand back a usable retry window. The transport carries
    * this into the typed error; when it was dropped, every suppressed call
    * reported the same synthetic floor regardless of the real backoff. */
   dependency_breaker_reset(&g_breaker);
   for (unsigned i = 0; i < DEPENDENCY_BREAKER_DEFAULT_THRESHOLD; i++)
      dependency_breaker_report_failure(&g_breaker, t0, DEPENDENCY_BREAKER_DEFAULT_THRESHOLD,
                                        DEPENDENCY_BREAKER_DEFAULT_BASE_MS,
                                        DEPENDENCY_BREAKER_DEFAULT_MAX_MS);
   int64_t denied_retry = -1;
   assert(dependency_breaker_allow(&g_breaker, t0, &denied_retry) == 0);
   assert(denied_retry > 0);

   printf("dependency_breaker: ok\n");
   return 0;
}
