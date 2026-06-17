/* server_delegate_ondemand.c: on-demand execution for I/O-bound delegates.
 *
 * Delegates spend ≈100% of their wall time blocked on a provider's HTTP
 * response, using no CPU and only their stack. Gating them on the small CPU
 * compute pool (background=2) and the 2-wide compute budget — a relic of the
 * era when the server embedded/indexed inline, before that work moved to the
 * embedder container and aimee-kb — meant a couple of slow or hung remote calls
 * wedged the entire background-delegate queue. Each delegate instead runs on
 * its own detached thread, reaped on completion. The only throttle that matters
 * (and stays) is the per-model/provider concurrency limiter; a high,
 * configurable ceiling backstops pathological fan-out (fd/memory).
 *
 * Split out of server_compute.c to keep that file under the 2000-line limit. */
#include "server_compute_impl.h"
#include "cJSON.h"
#include "config.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <time.h>

#define DELEGATE_ONDEMAND_CEILING_DEFAULT 512
#define DELEGATE_ONDEMAND_THREAD_STACK    ((size_t)32 * 1024 * 1024)

static pthread_mutex_t g_delegate_inflight_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_delegate_inflight_cond = PTHREAD_COND_INITIALIZER;
static int g_delegate_inflight = 0;
static int g_delegate_ceiling = DELEGATE_ONDEMAND_CEILING_DEFAULT;
/* Cumulative on-demand delegates admitted (≈ spawned) and the peak concurrent
 * in-flight count, for `aimee workers` diagnostics: together with the live
 * gauge they let an operator confirm in production that the per-model limiter —
 * not a thread cap — is the binding throttle (the high-water rises above the old
 * 2-thread pool ceiling), which is the whole point of on-demand execution. */
static long g_delegate_spawned_total = 0;
static int g_delegate_inflight_high_water = 0;

void delegate_ondemand_set_ceiling(int ceiling)
{
   if (ceiling <= 0)
      return;
   pthread_mutex_lock(&g_delegate_inflight_mutex);
   g_delegate_ceiling = ceiling;
   pthread_mutex_unlock(&g_delegate_inflight_mutex);
}

int delegate_ondemand_inflight(void)
{
   pthread_mutex_lock(&g_delegate_inflight_mutex);
   int n = g_delegate_inflight;
   pthread_mutex_unlock(&g_delegate_inflight_mutex);
   return n;
}

/* Add the on-demand delegate diagnostics to an `aimee workers` response object:
 * the live in-flight count, its peak, and the cumulative total. Read together
 * under one lock for a consistent snapshot. Kept here (not in server_state.c)
 * so the counters stay private to this module and server_state.c stays under
 * its line limit. */
void delegate_ondemand_add_workers_stats(cJSON *out)
{
   if (!out)
      return;
   pthread_mutex_lock(&g_delegate_inflight_mutex);
   int inflight = g_delegate_inflight;
   int high_water = g_delegate_inflight_high_water;
   long spawned_total = g_delegate_spawned_total;
   pthread_mutex_unlock(&g_delegate_inflight_mutex);
   cJSON_AddNumberToObject(out, "delegates_inflight", inflight);
   cJSON_AddNumberToObject(out, "delegates_inflight_high_water", high_water);
   cJSON_AddNumberToObject(out, "delegates_spawned_total", (double)spawned_total);
}

static void delegate_inflight_dec(void)
{
   pthread_mutex_lock(&g_delegate_inflight_mutex);
   if (g_delegate_inflight > 0)
      g_delegate_inflight--;
   pthread_cond_broadcast(&g_delegate_inflight_cond);
   pthread_mutex_unlock(&g_delegate_inflight_mutex);
}

static void *delegate_ondemand_thread(void *arg)
{
   compute_ctx_t *cctx = (compute_ctx_t *)arg;
   delegate_worker(cctx); /* runs to completion and frees cctx */
   delegate_inflight_dec();
   return NULL;
}

int delegate_spawn_ondemand(compute_ctx_t *cctx)
{
   if (!cctx)
      return -1;
   /* Bypass the 2-wide compute budget exactly like session delegates do: a
    * positive executor-thread grant makes compute_ctx_begin_budget take its
    * non-blocking path (see compute_ctx_begin_budget / _release_budget). The
    * per-model limiter, not the budget, governs delegate concurrency. */
   if (cctx->compute_executor_threads <= 0)
      cctx->compute_executor_threads = CONFIG_DEFAULT_SESSION_THREADS;

   pthread_mutex_lock(&g_delegate_inflight_mutex);
   if (g_delegate_inflight >= g_delegate_ceiling)
   {
      pthread_mutex_unlock(&g_delegate_inflight_mutex);
      LOG_WARN("server.delegate", "on-demand delegate ceiling reached (%d in flight)",
               g_delegate_ceiling);
      return -1;
   }
   g_delegate_inflight++;
   g_delegate_spawned_total++;
   if (g_delegate_inflight > g_delegate_inflight_high_water)
      g_delegate_inflight_high_water = g_delegate_inflight;
   pthread_mutex_unlock(&g_delegate_inflight_mutex);

   pthread_attr_t attr;
   pthread_attr_t *attr_p = NULL;
   if (pthread_attr_init(&attr) == 0)
   {
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      pthread_attr_setstacksize(&attr, DELEGATE_ONDEMAND_THREAD_STACK);
      attr_p = &attr;
   }
   pthread_t tid;
   int rc = pthread_create(&tid, attr_p, delegate_ondemand_thread, cctx);
   if (attr_p)
      pthread_attr_destroy(attr_p);
   if (rc != 0)
   {
      /* Admitted but the thread never started: roll back the in-flight
       * reservation AND the cumulative spawned count, so spawned_total only ever
       * reflects delegates that actually ran (no overcount on create failure). */
      pthread_mutex_lock(&g_delegate_inflight_mutex);
      if (g_delegate_inflight > 0)
         g_delegate_inflight--;
      if (g_delegate_spawned_total > 0)
         g_delegate_spawned_total--;
      pthread_cond_broadcast(&g_delegate_inflight_cond);
      pthread_mutex_unlock(&g_delegate_inflight_mutex);
      LOG_ERROR("server.delegate", "failed to spawn on-demand delegate thread: %d", rc);
      return -1;
   }
   return 0;
}

void delegate_ondemand_drain(int timeout_ms)
{
   struct timespec deadline;
   clock_gettime(CLOCK_REALTIME, &deadline);
   deadline.tv_sec += timeout_ms / 1000;
   deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
   if (deadline.tv_nsec >= 1000000000L)
   {
      deadline.tv_sec += 1;
      deadline.tv_nsec -= 1000000000L;
   }
   pthread_mutex_lock(&g_delegate_inflight_mutex);
   while (g_delegate_inflight > 0)
   {
      if (pthread_cond_timedwait(&g_delegate_inflight_cond, &g_delegate_inflight_mutex,
                                 &deadline) == ETIMEDOUT)
         break;
   }
   int remaining = g_delegate_inflight;
   pthread_mutex_unlock(&g_delegate_inflight_mutex);
   if (remaining > 0)
      LOG_WARN("server.delegate", "shutdown: %d on-demand delegate(s) still in flight after drain",
               remaining);
}
