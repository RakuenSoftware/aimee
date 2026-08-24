/* db1_store_probe.c: can the store be USED right now, not merely reached?
 *
 * db1_store_ready() answers "is a module serving the store", which is what
 * every store-backed command wants: it is a precondition check on the way to a
 * call that will report its own failure, and it must stay cheap because it runs
 * before all of them.
 *
 * The health endpoint is asking a different question, and the difference is
 * visible for about half a minute. Module availability is registry state, and
 * the registry is corrected by the bus's heartbeat reaper: 30s of staleness
 * plus a reap that runs every 7.5s. Kill the module and, measured twice on a
 * clean container, /v1/server/health kept answering "ok" for 36.5s and 37s
 * while every store call was already failing.
 *
 * That window is the beginning of an outage -- the part someone is watching,
 * and the part a container healthcheck would have to catch to restart anything.
 * A health endpoint that disagrees with the calls beside it during exactly that
 * window is worse than a slow one, so this asks the store a real question.
 *
 * The question is server_session_count over an impossible window: it is
 * declared safe, takes one bound parameter, touches one indexed table and
 * returns a scalar. It is not free, which is why the answer is cached for a
 * second -- long enough that polling health cannot turn into load on the
 * module, short enough that the reported state and the store's actual state
 * cannot diverge in a way anybody could act on.
 */
#include "db1_client/db1.h"
#include "db1_module_api.h"
#include "db1_client/server_sessions.h"

#include <pthread.h>
#include <time.h>

#include <aimee/audit/obs_bus.h>

/* A window nothing can fall into, so the probe reads an index and returns zero
 * rather than counting rows on a busy store. */
#define PROBE_SINCE    "9999-12-31T23:59:59Z"
#define PROBE_CACHE_NS 1000000000LL /* 1s */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static long long g_checked_at_ns;
static int g_last_answer = -1;

static long long now_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int db1_store_probe(void)
{
   /* No module registered at all is already the answer, and asking costs a
    * timeout rather than a round trip. This is also what keeps the probe from
    * running on a daemon that never had a store. */
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_SESSIONS))
   {
      pthread_mutex_lock(&g_lock);
      g_last_answer = 0;
      g_checked_at_ns = now_ns();
      pthread_mutex_unlock(&g_lock);
      return 0;
   }

   long long now = now_ns();
   pthread_mutex_lock(&g_lock);
   if (g_last_answer >= 0 && g_checked_at_ns != 0 && now - g_checked_at_ns < PROBE_CACHE_NS)
   {
      int cached = g_last_answer;
      pthread_mutex_unlock(&g_lock);
      return cached;
   }
   pthread_mutex_unlock(&g_lock);

   /* Outside the lock: a probe that blocks on the module must not also block
    * every other caller asking the same question. Two concurrent probes cost
    * one extra round trip, which is cheaper than serialising on the module's
    * latency. */
   int count = db1_server_session_count(PROBE_SINCE);
   int answer = (count >= 0) ? 1 : 0;

   pthread_mutex_lock(&g_lock);
   g_last_answer = answer;
   g_checked_at_ns = now_ns();
   pthread_mutex_unlock(&g_lock);
   return answer;
}
