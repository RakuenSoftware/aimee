/* test_server_ready.c: the readiness decision behind GET /v1/ready.
 *
 * Exercises server_ready_render() — the pure snapshot→(body,status) function —
 * by passing an explicit clock, so staleness and roll-up behavior are tested
 * deterministically rather than by sleeping past a real sampling interval.
 *
 * The sampler's own I/O (kb_client_health / db1_store_probe) is stubbed:
 * this suite is about the decision, not about reaching the dependencies. */
#include "server_http.h"
#include "kb_client.h"
#include <aimee/git/module_api.h>
#include <aimee/runtime-web/module_api.h>
#include <aimee/skills/module_api.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/* --- stubs for the sampler's dependency closure (link-only) --- */
static atomic_int g_db1_probe_calls;
static atomic_int g_kb_probe_calls;
static int g_kb_configured;
static pthread_mutex_t g_probe_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_probe_cond = PTHREAD_COND_INITIALIZER;
static int g_block_probe, g_probe_entered, g_stop_returned;
static _Atomic(server_http_ready_fn) g_provider;
int kb_client_connection_configured(void)
{
   return g_kb_configured;
}

int db1_store_probe(void)
{
   g_db1_probe_calls++;
   pthread_mutex_lock(&g_probe_mutex);
   g_probe_entered = 1;
   pthread_cond_broadcast(&g_probe_cond);
   while (g_block_probe)
      pthread_cond_wait(&g_probe_cond, &g_probe_mutex);
   pthread_mutex_unlock(&g_probe_mutex);
   return 1;
}

int kb_client_health(kb_health_t *out)
{
   g_kb_probe_calls++;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

void server_http_set_ready_provider(server_http_ready_fn fn)
{
   atomic_store(&g_provider, fn);
}

void kb_client_dependency_health(kb_client_dependency_health_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->state, sizeof(out->state), "closed");
}

static atomic_int g_runtime_web_checked;
static atomic_int g_module_probe_calls;
static atomic_int g_skills_trigger_checked;
static atomic_int g_git_ref_checked;

int obs_bus_module_available(uint32_t event_kind)
{
   g_module_probe_calls++;
   if (event_kind == AIMEE_RUNTIME_WEB_EVENT_CLASSIFY)
      g_runtime_web_checked = 1;
   if (event_kind == AIMEE_SKILLS_EVENT_TRIGGER)
      g_skills_trigger_checked = 1;
   if (event_kind == AIMEE_GIT_EVENT_REF_VALIDATE)
      g_git_ref_checked = 1;
   return 1;
}

/* Observe dependency traffic and completed stop calls, rather than sampler
 * implementation details. Every wait has a test-only wall-clock bound. */
static struct timespec deadline_ms(long ms)
{
   struct timespec t;
   assert(clock_gettime(CLOCK_REALTIME, &t) == 0);
   t.tv_sec += ms / 1000;
   t.tv_nsec += (ms % 1000) * 1000000L;
   if (t.tv_nsec >= 1000000000L)
   {
      t.tv_sec++;
      t.tv_nsec -= 1000000000L;
   }
   return t;
}

static int wait_flag(int *flag, long ms)
{
   struct timespec end = deadline_ms(ms);
   pthread_mutex_lock(&g_probe_mutex);
   while (!*flag)
   {
      int rc = pthread_cond_timedwait(&g_probe_cond, &g_probe_mutex, &end);
      if (rc == ETIMEDOUT)
         break;
      assert(rc == 0);
   }
   int observed = *flag;
   pthread_mutex_unlock(&g_probe_mutex);
   return observed;
}

static void wait_ready(void)
{
   struct timespec end = deadline_ms(2000), now;
   char body[2048];
   for (;;)
   {
      server_http_ready_fn fn = atomic_load(&g_provider);
      assert(fn);
      if (fn(body, sizeof body) == 200)
         return;
      assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
      assert(now.tv_sec < end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec < end.tv_nsec));
      struct timespec pause = {.tv_nsec = 1000000};
      nanosleep(&pause, NULL);
   }
}

static void *stop_sampler(void *arg)
{
   (void)arg;
   server_ready_stop();
   pthread_mutex_lock(&g_probe_mutex);
   g_stop_returned = 1;
   pthread_cond_broadcast(&g_probe_cond);
   pthread_mutex_unlock(&g_probe_mutex);
   return NULL;
}

static void *cycle_sampler(void *arg)
{
   (void)arg;
   for (int i = 0; i < 16; i++)
   {
      server_ready_register();
      server_ready_stop();
   }
   return NULL;
}

static void test_sampler_lifecycle(void)
{
   /* Registration remains nonblocking even while its first dependency is held.
    * Stop must join that in-flight sample before its callers retire the bus. */
   setenv("AIMEE_READY_INTERVAL_SECS", "3600", 1);
   pthread_mutex_lock(&g_probe_mutex);
   g_block_probe = 1;
   g_probe_entered = g_stop_returned = 0;
   pthread_mutex_unlock(&g_probe_mutex);
   server_ready_register();
   assert(wait_flag(&g_probe_entered, 2000));
   char body[2048];
   int probes = atomic_load(&g_db1_probe_calls);
   int first_status = atomic_load(&g_provider)(body, sizeof body);
   assert(atomic_load(&g_db1_probe_calls) == probes); /* render never probes */
   pthread_t stopper;
   assert(pthread_create(&stopper, NULL, stop_sampler, NULL) == 0);
   assert(!wait_flag(&g_stop_returned, 100));
   assert(first_status == 503);
   pthread_mutex_lock(&g_probe_mutex);
   g_block_probe = 0;
   pthread_cond_broadcast(&g_probe_cond);
   pthread_mutex_unlock(&g_probe_mutex);
   assert(wait_flag(&g_stop_returned, 2000));
   pthread_join(stopper, NULL);
   assert(atomic_load(&g_provider)(body, sizeof body) == 503);

   /* A 3600-second interval must be interruptible, not a sleep that stop joins. */
   g_stop_returned = 0;
   server_ready_register();
   wait_ready();
   assert(pthread_create(&stopper, NULL, stop_sampler, NULL) == 0);
   assert(wait_flag(&g_stop_returned, 2000));
   pthread_join(stopper, NULL);
   server_ready_stop(); /* idempotent */

   /* Cross a real normal sampling deadline after stopping. Neither DB/KB nor
    * any module may be called again by the retired worker. */
   setenv("AIMEE_READY_INTERVAL_SECS", "1", 1);
   server_ready_register();
   wait_ready();
   server_ready_stop();
   probes = atomic_load(&g_db1_probe_calls);
   int kb = atomic_load(&g_kb_probe_calls), modules = atomic_load(&g_module_probe_calls);
   struct timespec end = deadline_ms(1250);
   pthread_mutex_lock(&g_probe_mutex);
   while (atomic_load(&g_db1_probe_calls) == probes)
   {
      int rc = pthread_cond_timedwait(&g_probe_cond, &g_probe_mutex, &end);
      if (rc == ETIMEDOUT)
         break;
      assert(rc == 0);
   }
   pthread_mutex_unlock(&g_probe_mutex);
   assert(atomic_load(&g_db1_probe_calls) == probes);
   assert(atomic_load(&g_kb_probe_calls) == kb);
   assert(atomic_load(&g_module_probe_calls) == modules);
   assert(atomic_load(&g_provider)(body, sizeof body) == 503);
   server_ready_sample_now(); /* explicit synchronous API remains usable */
   assert(atomic_load(&g_db1_probe_calls) == probes + 1);
   /* Concurrent owners cannot detach a worker or race start against its join. */
   pthread_t cycles[2];
   for (int i = 0; i < 2; i++)
      assert(pthread_create(&cycles[i], NULL, cycle_sampler, NULL) == 0);
   for (int i = 0; i < 2; i++)
      pthread_join(cycles[i], NULL);
   server_ready_stop();
   assert(atomic_load(&g_provider)(body, sizeof body) == 503);
   unsetenv("AIMEE_READY_INTERVAL_SECS");
}

#define NOW 1000000L

int main(void)
{
   char resp[2048];

   unsetenv("AIMEE_RUNTIME_WEB_ENABLED");
   unsetenv("AIMEE_MODULE_RUNTIME_WEB");
   server_ready_sample_now();
   assert(g_db1_probe_calls == 1);
   assert(g_kb_probe_calls == 0);
   g_kb_configured = 1;
   server_ready_sample_now();
   assert(g_kb_probe_calls == 1);
   g_kb_configured = 0;
   assert(g_runtime_web_checked == 1);
   assert(g_skills_trigger_checked == 1);
   assert(g_git_ref_checked == 1);

   /* runtime-web is optional. Readiness must follow the same operator intent
    * as the entrypoint instead of waiting forever for an intentionally
    * disabled module. An explicit module switch wins over the UI switch. */
   g_runtime_web_checked = 0;
   setenv("AIMEE_RUNTIME_WEB_ENABLED", "0", 1);
   server_ready_sample_now();
   assert(g_runtime_web_checked == 0);

   g_runtime_web_checked = 0;
   setenv("AIMEE_MODULE_RUNTIME_WEB", "yes", 1);
   server_ready_sample_now();
   assert(g_runtime_web_checked == 1);

   g_runtime_web_checked = 0;
   setenv("AIMEE_RUNTIME_WEB_ENABLED", "true", 1);
   setenv("AIMEE_MODULE_RUNTIME_WEB", "off", 1);
   server_ready_sample_now();
   assert(g_runtime_web_checked == 0);

   unsetenv("AIMEE_RUNTIME_WEB_ENABLED");
   unsetenv("AIMEE_MODULE_RUNTIME_WEB");
   server_ready_diagnostics_t ok = {.retrieval_ok = 1,
                                    .modules_ok = 1,
                                    .failed_boundary = "",
                                    .missing_module = "",
                                    .breaker_state = "closed",
                                    .retry_after_ms = 0,
                                    .last_success_query_ms = 999000,
                                    .last_ingest_at = "2026-07-30T00:00:00Z"};
   server_ready_diagnostics_t failed = {.retrieval_ok = 0,
                                        .modules_ok = 1,
                                        .failed_boundary = "kb_breaker",
                                        .missing_module = "",
                                        .breaker_state = "open",
                                        .retry_after_ms = 1200,
                                        .last_success_query_ms = 998000,
                                        .last_ingest_at = "2026-07-30T00:00:00Z"};

   /* A standalone Server is ready with its local store and required modules.
    * Local retrieval requires the store and modules; shared KB stays disabled. */
   {
      server_ready_diagnostics_t local = ok;
      local.kb_disabled = 1;
      local.retrieval_ok = 1;
      assert(server_ready_render(1, 0, &local, NOW - 5, NOW, 60, resp, sizeof(resp)) == 200);
      assert(strstr(resp, "\"kb\":\"disabled\""));
      assert(strstr(resp, "\"retrieval\":\"ok\""));
      local.retrieval_ok = 0;
      assert(server_ready_render(1, 0, &local, NOW - 5, NOW, 60, resp, sizeof(resp)) == 503);
      local.retrieval_ok = 1;
      assert(server_ready_render(0, 0, &local, NOW - 5, NOW, 60, resp, sizeof(resp)) == 503);
      assert(server_ready_render(1, 0, &local, NOW - 61, NOW, 60, resp, sizeof(resp)) == 503);
   }

   /* Never sampled ⇒ unknown, not ready, and a null age rather than a
    * fabricated one. An unsampled server must never read as ready. */
   {
      int st = server_ready_render(-1, -1, NULL, 0, NOW, 60, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"ready\":false"));
      assert(strstr(resp, "\"status\":\"unknown\""));
      assert(strstr(resp, "\"sampled_at\":null"));
      assert(strstr(resp, "\"age_seconds\":null"));
   }
   {
      int st = server_ready_render(1, 1, &failed, NOW - 5, NOW, 60, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"status\":\"degraded\""));
      assert(strstr(resp, "\"kb\":\"ok\""));
      assert(strstr(resp, "\"retrieval\":\"fail\""));
      assert(strstr(resp, "\"failed_boundary\":\"kb_breaker\""));
   }

   /* Dependency text cannot break the JSON diagnostics object. */
   {
      server_ready_diagnostics_t escaped = ok;
      escaped.last_ingest_at = "quote\" slash\\ newline\n";
      int st = server_ready_render(1, 1, &escaped, NOW - 5, NOW, 60, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "quote\\\" slash\\\\ newline\\u000a"));
   }

   /* Fresh sample, everything ok ⇒ ready. */
   {
      int st = server_ready_render(1, 1, &ok, NOW - 5, NOW, 60, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"ready\":true"));
      assert(strstr(resp, "\"status\":\"ok\""));
      assert(strstr(resp, "\"db1\":\"ok\""));
      assert(strstr(resp, "\"kb\":\"ok\""));
      assert(strstr(resp, "\"retrieval\":\"ok\""));
      assert(strstr(resp, "\"modules\":\"ok\""));
      assert(strstr(resp, "\"last_success_query_ms\":999000"));
      assert(strstr(resp, "\"age_seconds\":5"));
   }

   /* A required process module that has not attached keeps the daemon out of
    * rotation even when its remote dependencies are healthy. */
   {
      server_ready_diagnostics_t detached = ok;
      detached.modules_ok = 0;
      detached.missing_module = "routing";
      int st = server_ready_render(1, 1, &detached, NOW - 5, NOW, 60, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"modules\":\"fail\""));
      assert(strstr(resp, "\"missing_module\":\"routing\""));
   }

   /* A fresh snapshot with missing diagnostics fails closed with valid,
    * deterministic JSON rather than exposing uninitialized stack text. */
   {
      int st = server_ready_render(1, 1, NULL, NOW - 5, NOW, 60, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"retrieval\":\"unknown\""));
      assert(strstr(resp, "\"failed_boundary\":\"\""));
      assert(strstr(resp, "\"breaker_state\":\"\""));
      assert(strstr(resp, "\"last_ingest_at\":\"\""));
   }

   /* One dependency down ⇒ not ready, degraded, and the failing dependency is
    * named rather than hidden behind a roll-up. */
   {
      int st = server_ready_render(1, 0, &failed, NOW - 5, NOW, 60, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"ready\":false"));
      assert(strstr(resp, "\"status\":\"degraded\""));
      assert(strstr(resp, "\"db1\":\"ok\""));
      assert(strstr(resp, "\"kb\":\"fail\""));
      assert(strstr(resp, "\"breaker_state\":\"open\""));
      assert(strstr(resp, "\"retry_after_ms\":1200"));
   }
   {
      int st = server_ready_render(0, 1, &ok, NOW - 5, NOW, 60, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"db1\":\"fail\""));
   }

   /* A stale snapshot degrades to unknown even when every dependency sampled
    * ok — a wedged sampler must not leave "ready" standing forever. */
   {
      int st = server_ready_render(1, 1, &ok, NOW - 61, NOW, 60, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"ready\":false"));
      assert(strstr(resp, "\"status\":\"unknown\""));
      assert(strstr(resp, "\"db1\":\"unknown\""));
      assert(strstr(resp, "\"kb\":\"unknown\""));
   }

   /* Exactly at the bound is still fresh (the bound is inclusive). */
   {
      int st = server_ready_render(1, 1, &ok, NOW - 60, NOW, 60, resp, sizeof(resp));
      assert(st == 200);
      assert(strstr(resp, "\"ready\":true"));
   }

   /* A snapshot stamped in the future means the clock moved; the age is
    * meaningless, so fail closed rather than report a negative age as fresh. */
   {
      int st = server_ready_render(1, 1, &ok, NOW + 30, NOW, 60, resp, sizeof(resp));
      assert(st == 503);
      assert(strstr(resp, "\"status\":\"unknown\""));
   }

   /* A healthy DB/module graph must not hide an almost-full audit volume. */
   server_ready_diagnostics_t capacity = ok;
   capacity.audit_capacity_failed = 1;
   capacity.failed_boundary = "audit_capacity";
   assert(server_ready_render(1, 1, &capacity, NOW - 5, NOW, 60, resp, sizeof(resp)) == 503);
   assert(strstr(resp, "\"ready\":false"));
   assert(strstr(resp, "audit_capacity"));
   capacity.audit_capacity_failed = 0;
   assert(server_ready_render(1, 1, &capacity, NOW - 5, NOW, 60, resp, sizeof(resp)) == 200);
   test_sampler_lifecycle();
   printf("server_ready: OK\n");
   return 0;
}
