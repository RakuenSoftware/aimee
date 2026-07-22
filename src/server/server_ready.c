/* server_ready.c: the readiness snapshot sampler behind GET /v1/ready.
 *
 * The route (server_http.c) must not perform dependency I/O — a wedged
 * dependency would otherwise stall the listener — so this unit samples each
 * dependency on a background interval and publishes an immutable snapshot the
 * route can read in O(1). This is also why the sampler lives here rather than
 * in server_http.c: it pulls the kb_client/db1 dependency closure that
 * server_http.c and its unit test deliberately stay free of.
 *
 * Dependencies sampled:
 *   db1  — db1_is_initialized(). Deliberately a no-I/O check, not the
 *          store/load/remove round-trip `aimee doctor` uses: doctor runs once
 *          on demand, this runs on a timer, and a periodic write probe would
 *          make the readiness endpoint its own source of load.
 *   kb   — kb_client_health(). One HTTP call to aimee-kb, off the request path.
 *
 * aimee-llm is intentionally NOT sampled here. The dependency chain is
 * server -> aimee-kb -> aimee-llm; the server holds no llm configuration, and
 * llm reachability is already reflected in aimee-kb's own health. Probing it
 * from here would assert a relationship the architecture does not have.
 *
 * Fail-closed rules:
 *   - before the first sample completes, every dependency is `unknown` and the
 *     server is NOT ready (an unsampled server must never read as ready);
 *   - a snapshot older than the staleness bound degrades back to `unknown`, so
 *     a wedged sampler cannot leave a stale "ready" standing forever;
 *   - the roll-up is ready only when every dependency sampled `ok`.
 *
 * Tuning (env, so an operator can adjust without a rebuild; conservative
 * defaults chosen so the probe never becomes load):
 *   AIMEE_READY_INTERVAL_SECS  sampling period      (default 15, min 1)
 *   AIMEE_READY_STALE_SECS     staleness bound      (default 60, min interval)
 */
#include "server_http.h"
#include "kb_client.h"
#include "db1.h"
#include "log.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum
{
   DEP_UNKNOWN = 0,
   DEP_OK,
   DEP_FAIL
} dep_state_t;

typedef struct
{
   dep_state_t db1;
   dep_state_t kb;
   long sampled_at; /* epoch seconds; 0 = never sampled */
} ready_snapshot_t;

static pthread_mutex_t g_ready_mtx = PTHREAD_MUTEX_INITIALIZER;
static ready_snapshot_t g_snap; /* guarded by g_ready_mtx */
static pthread_t g_ready_thread;
static int g_ready_thread_started;

#define READY_INTERVAL_DEFAULT 15
#define READY_STALE_DEFAULT    60

static int env_int(const char *name, int dflt, int min)
{
   const char *v = getenv(name);
   if (!v || !v[0])
      return dflt;
   int n = atoi(v);
   return (n < min) ? min : n;
}

static int ready_interval_secs(void)
{
   return env_int("AIMEE_READY_INTERVAL_SECS", READY_INTERVAL_DEFAULT, 1);
}

static int ready_stale_secs(void)
{
   return env_int("AIMEE_READY_STALE_SECS", READY_STALE_DEFAULT, ready_interval_secs());
}

static const char *dep_name(dep_state_t s)
{
   switch (s)
   {
   case DEP_OK:
      return "ok";
   case DEP_FAIL:
      return "fail";
   default:
      return "unknown";
   }
}

/* Sample every dependency into a local snapshot, then publish it under the
 * lock in one assignment so a concurrent reader sees the previous snapshot or
 * this one, never a half-written mix. */
void server_ready_sample_now(void)
{
   ready_snapshot_t s;
   memset(&s, 0, sizeof(s));

   s.db1 = db1_is_initialized() ? DEP_OK : DEP_FAIL;

   kb_health_t h;
   memset(&h, 0, sizeof(h));
   s.kb = (kb_client_health(&h) == 0 && h.process_ok) ? DEP_OK : DEP_FAIL;

   s.sampled_at = (long)time(NULL);

   pthread_mutex_lock(&g_ready_mtx);
   g_snap = s;
   pthread_mutex_unlock(&g_ready_mtx);
}

static void *ready_sampler_main(void *arg)
{
   (void)arg;
   for (;;)
   {
      server_ready_sample_now();
      sleep((unsigned)ready_interval_secs());
   }
   return NULL;
}

/* The readiness decision, as a pure function of a snapshot and a clock: no
 * globals, no locks, no I/O. Split out so staleness and roll-up behavior can be
 * tested deterministically by passing a `now` rather than sleeping past a real
 * interval. `db1_ok`/`kb_ok` are 1 ok, 0 fail, -1 unknown/not-sampled. */
int server_ready_render(int db1_ok, int kb_ok, long sampled_at, long now, int stale_secs,
                        char *resp, int cap)
{
   dep_state_t db1 = (db1_ok > 0) ? DEP_OK : (db1_ok == 0 ? DEP_FAIL : DEP_UNKNOWN);
   dep_state_t kb = (kb_ok > 0) ? DEP_OK : (kb_ok == 0 ? DEP_FAIL : DEP_UNKNOWN);

   long age = (sampled_at > 0) ? (now - sampled_at) : -1;

   /* Never sampled, or too old to trust — including a snapshot stamped in the
    * future, which means the clock moved and the age is meaningless. Degrade to
    * unknown so a wedged sampler cannot keep advertising a stale "ready". */
   int stale = (sampled_at <= 0) || (age < 0) || (age > (long)stale_secs);
   if (stale)
   {
      db1 = DEP_UNKNOWN;
      kb = DEP_UNKNOWN;
   }

   int ready = (db1 == DEP_OK && kb == DEP_OK);
   const char *status = ready ? "ok" : (stale ? "unknown" : "degraded");

   if (stale && (sampled_at <= 0 || age < 0))
      snprintf(resp, (size_t)cap,
               "{\"ready\":false,\"status\":\"unknown\",\"service\":\"aimee-server\","
               "\"sampled_at\":null,\"age_seconds\":null,"
               "\"dependencies\":{\"db1\":\"unknown\",\"kb\":\"unknown\"}}");
   else
      snprintf(resp, (size_t)cap,
               "{\"ready\":%s,\"status\":\"%s\",\"service\":\"aimee-server\","
               "\"sampled_at\":%ld,\"age_seconds\":%ld,"
               "\"dependencies\":{\"db1\":\"%s\",\"kb\":\"%s\"}}",
               ready ? "true" : "false", status, sampled_at, age, dep_name(db1), dep_name(kb));

   return ready ? 200 : 503;
}

/* Serve the snapshot. Performs no I/O: a locked copy, then a pure render. */
static int ready_provider(char *resp, int cap)
{
   pthread_mutex_lock(&g_ready_mtx);
   ready_snapshot_t s = g_snap;
   pthread_mutex_unlock(&g_ready_mtx);

   int db1_ok = (s.db1 == DEP_UNKNOWN) ? -1 : (s.db1 == DEP_OK);
   int kb_ok = (s.kb == DEP_UNKNOWN) ? -1 : (s.kb == DEP_OK);

   return server_ready_render(db1_ok, kb_ok, s.sampled_at, (long)time(NULL), ready_stale_secs(),
                              resp, cap);
}

/* Take the first sample synchronously so a server that is already healthy does
 * not advertise `unknown` for one interval, then run the sampler in the
 * background. Registration is last: until it happens the route's own
 * fail-closed path answers 503. */
void server_ready_register(void)
{
   server_ready_sample_now();

   if (!g_ready_thread_started)
   {
      if (pthread_create(&g_ready_thread, NULL, ready_sampler_main, NULL) == 0)
      {
         pthread_detach(g_ready_thread);
         g_ready_thread_started = 1;
      }
      else
      {
         /* No sampler means the snapshot can only go stale. Leaving the
          * provider registered would then answer `unknown`/503 forever, which
          * is the correct fail-closed answer, but say so rather than fail
          * silently. */
         LOG_ERROR("ready", "readiness sampler thread failed to start");
      }
   }

   server_http_set_ready_provider(ready_provider);
}
