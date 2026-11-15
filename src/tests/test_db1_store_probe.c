/* Module transport success cannot attest that SQL operations succeeded. */
#include "db1_client/db1.h"
#include <aimee/audit/obs_bus.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int attached = 1, count_result = 0, probes;
static int64_t fake_ns = 10000000000LL;
int clock_gettime(clockid_t clock, struct timespec *value)
{
   (void)clock;
   value->tv_sec = fake_ns / 1000000000LL;
   value->tv_nsec = fake_ns % 1000000000LL;
   return 0;
}
int obs_bus_module_available(uint32_t kind)
{
   (void)kind;
   return attached;
}
void obs_bus_module_outcome(uint32_t ref, int64_t *ok, int64_t *failed)
{
   (void)ref;
   *ok = fake_ns - 1; /* transport delivered an application-level SQL error */
   *failed = 0;
}
int db1_server_session_count(const char *since)
{
   assert(strcmp(since, "9999-12-31T23:59:59Z") == 0);
   probes++;
   return count_result;
}
int main(void)
{
   assert(db1_store_probe() && probes == 1);
   count_result = -1;
   assert(db1_store_probe() && probes == 1); /* bounded one-second cache */
   fake_ns += 1000000001LL;
   assert(!db1_store_probe() && probes == 2);
   count_result = 0;
   fake_ns += 1000000001LL;
   assert(db1_store_probe() && probes == 3);
   attached = 0;
   assert(!db1_store_probe() && probes == 3);
   attached = 1;
   fake_ns += 1000000001LL;
   assert(db1_store_probe() && probes == 4);
   puts("store readiness: SQL failure, bounded cache, recovery and absent module passed");
   return 0;
}
