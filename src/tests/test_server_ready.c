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
#include <time.h>

/* --- stubs for the sampler's dependency closure (link-only) --- */
static _Atomic int g_db1_probe_calls; /* the sampler thread probes too */
static int g_kb_probe_calls;
static int g_kb_configured;
int kb_client_connection_configured(void)
{
   return g_kb_configured;
}

int db1_store_probe(void)
{
   g_db1_probe_calls++;
   return 1;
}

int kb_client_health(kb_health_t *out)
{
   g_kb_probe_calls++;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

static server_http_ready_fn g_provider;
void server_http_set_ready_provider(server_http_ready_fn fn)
{
   g_provider = fn;
}

static void pause_ms(long ms)
{
   struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
   nanosleep(&t, NULL);
}

/* The sampler samples on demand, never on a timer: idle, it asks nothing. */
static void test_samples_only_when_asked(void)
{
   char resp[2048];
   /* Ages are whole seconds; every step below keeps a full second of margin. */
   setenv("AIMEE_READY_INTERVAL_SECS", "2", 1);
   setenv("AIMEE_READY_STALE_SECS", "4", 1);
   int before = g_db1_probe_calls;
   server_ready_register();
   assert(g_provider != NULL);
   for (int i = 0; i < 200 && g_db1_probe_calls == before; i++)
      pause_ms(10);
   assert(g_db1_probe_calls == before + 1); /* the startup sample */

   /* Nobody asks: nothing is sampled, however long it is left. */
   pause_ms(5200); /* age >= 5, past the 4 s stale bound */
   assert(g_db1_probe_calls == before + 1);

   /* Too old to answer from: the request waits for the sample it asked for. */
   (void)g_provider(resp, sizeof resp);
   assert(g_db1_probe_calls == before + 2);
   assert(strstr(resp, "\"db1\":\"ok\""));

   /* Fresh (age 0 or 1, under the 2 s interval): answered from the snapshot. */
   (void)g_provider(resp, sizeof resp);
   assert(strstr(resp, "\"db1\":\"ok\""));
   pause_ms(100);
   assert(g_db1_probe_calls == before + 2);

   /* Due but still answerable (age 2 or 3): answered at once, the sample
    * lands behind it. */
   pause_ms(2100);
   (void)g_provider(resp, sizeof resp);
   assert(strstr(resp, "\"db1\":\"ok\"")); /* answered from the last sample */
   for (int i = 0; i < 200 && g_db1_probe_calls == before + 2; i++)
      pause_ms(10);
   assert(g_db1_probe_calls == before + 3);
   unsetenv("AIMEE_READY_INTERVAL_SECS");
   unsetenv("AIMEE_READY_STALE_SECS");
   puts("  readiness samples on demand, never on a timer");
}

void kb_client_dependency_health(kb_client_dependency_health_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->state, sizeof(out->state), "closed");
}

static int g_runtime_web_checked;
static int g_skills_trigger_checked;
static int g_git_ref_checked;

int obs_bus_module_available(uint32_t event_kind)
{
   if (event_kind == AIMEE_RUNTIME_WEB_EVENT_CLASSIFY)
      g_runtime_web_checked = 1;
   if (event_kind == AIMEE_SKILLS_EVENT_TRIGGER)
      g_skills_trigger_checked = 1;
   if (event_kind == AIMEE_GIT_EVENT_REF_VALIDATE)
      g_git_ref_checked = 1;
   return 1;
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
   test_samples_only_when_asked();
   printf("server_ready: OK\n");
   return 0;
}
