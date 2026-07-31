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
   printf("dependency_breaker: ok\n");
   return 0;
}
