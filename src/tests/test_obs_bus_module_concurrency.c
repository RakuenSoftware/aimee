/* test_obs_bus_module_concurrency.c: concurrent C->module calls must not
 * serialize behind one another.
 *
 * A module call is synchronous and holds its client for the whole request and
 * reply. While every caller shared one client, a long stage blocked every other
 * module call in the process. That is not a throughput concern, it is a
 * deadlock: a roundtable review held the client for minutes, and the module
 * running that review called back into this same server to launch its seats --
 * a callback that needed a client and could never get one. The review waited on
 * itself and only a remote timeout broke it.
 *
 * These tests pin the two properties that make that impossible:
 *   1. Calls proceed concurrently rather than one at a time.
 *   2. A caller never waits past its own deadline for a client, so pool
 *      exhaustion is reported as a deadline instead of becoming a hang. */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

void audit_log_open(void);

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* No module is attached, so every call resolves to capability_absent. That is
 * exactly what this test wants: the outcome is uninteresting, the concurrency
 * of getting there is the point. */
#define ABSENT_KIND  5889u
#define ABSENT_STAGE 1u

enum
{
   CALLERS = 8,
   CALLS_PER_CALLER = 50
};

static void *call_many(void *arg)
{
   (void)arg;
   for (int i = 0; i < CALLS_PER_CALLER; ++i)
   {
      char response[16];
      uint32_t response_len = 0;
      aimee_module_call_result_t result =
          obs_bus_module_call(ABSENT_KIND, ABSENT_STAGE, 1, now_ns() + 5000000000ULL, "probe", 5,
                              response, sizeof response, &response_len, NULL, NULL);
      assert(result == AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
   }
   return NULL;
}

/* The property is overlap, not speed.
 *
 * While one client was shared, its mutex spanned the whole request and reply,
 * so no two calls could ever be in flight at once -- maximum overlap was
 * exactly 1 by construction. Comparing wall time to the sum of call durations
 * does NOT detect that: a queued caller's own measurement includes the time it
 * spent waiting, so the sum inflates under serialization and the comparison
 * stays true either way. Counting genuinely overlapping intervals is the
 * difference that cannot be faked. */
static void test_calls_overlap(void)
{
   pthread_t threads[CALLERS];
   for (int i = 0; i < CALLERS; ++i)
      assert(pthread_create(&threads[i], NULL, call_many, NULL) == 0);
   for (int i = 0; i < CALLERS; ++i)
      pthread_join(threads[i], NULL);

   /* Ask the bus how many calls ever held a client at once. Measuring this from
    * outside does not work: a caller blocked waiting for a client is inside the
    * call the whole time, so wall-clock spans overlap just as much when every
    * call is serialized -- more, in fact, because everyone is queued. */
   int occupancy = obs_bus_module_peak_concurrency();
   printf("  %d calls across %d callers, peak client occupancy=%d\n", CALLERS * CALLS_PER_CALLER,
          CALLERS, occupancy);
   assert(occupancy > 1);
}

/* A caller that cannot get a client within its own deadline must be told so.
 * Waiting past the deadline is precisely the behaviour that turned a busy pool
 * into an unbounded hang. */
static void test_expired_deadline_does_not_block(void)
{
   char response[16];
   uint32_t response_len = 0;
   uint64_t start = now_ns();
   aimee_module_call_result_t result =
       obs_bus_module_call(ABSENT_KIND, ABSENT_STAGE, 1, now_ns() - 1, "probe", 5, response,
                           sizeof response, &response_len, NULL, NULL);
   uint64_t elapsed = now_ns() - start;
   assert(result == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   /* "Returned promptly" is the claim; a second is orders of magnitude more
    * than any bookkeeping here needs and still catches a blocking wait. */
   assert(elapsed < 1000000000ULL);
}

int main(void)
{
   printf("obs_bus_module_concurrency:\n");
   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busconc-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();
   if (obs_bus_start() != 0)
   {
      fprintf(stderr, "FAIL: obs_bus_start\n");
      return 1;
   }

   test_calls_overlap();
   test_expired_deadline_does_not_block();

   obs_bus_stop();
   printf("ok\n");
   return 0;
}
