/* test_server_ready.c: the readiness decision behind GET /v1/ready.
 *
 * Exercises server_ready_render() — the pure snapshot→(body,status) function —
 * by passing an explicit clock, so staleness and roll-up behavior are tested
 * deterministically rather than by sleeping past a real sampling interval.
 *
 * The sampler's own I/O (kb_client_health / db1_is_initialized) is stubbed:
 * this suite is about the decision, not about reaching the dependencies. */
#include "server_http.h"
#include "kb_client.h"
#include <aimee/git/module_api.h>
#include <aimee/runtime-web/module_api.h>
#include <aimee/skills/module_api.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* --- stubs for the sampler's dependency closure (link-only) --- */
int db1_is_initialized(void)
{
   return 1;
}

int kb_client_health(kb_health_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

void server_http_set_ready_provider(server_http_ready_fn fn)
{
   (void)fn;
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
   server_ready_sample_now();
   assert(g_runtime_web_checked == 1);
   assert(g_skills_trigger_checked == 1);
   assert(g_git_ref_checked == 1);
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

   printf("server_ready: OK\n");
   return 0;
}
