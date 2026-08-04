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
 *   modules — required same-container process modules attached to the local bus.
 *
 * The retrieval contract additionally requires the KB schema/vector collection
 * and embedder advertised by KB health, plus a non-open E5a transport breaker.
 * This is readiness, not liveness: a recoverable dependency outage drains work
 * without asking the supervisor to restart a healthy server process.
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
 *   AIMEE_READY_INTERVAL_SECS  sampling period  (default 15, range 1..3600)
 *   AIMEE_READY_STALE_SECS     staleness bound  (default 60, range interval..86400)
 */
#include "server_http.h"
#include "kb_client.h"
#include "db1.h"
#include "log.h"
#include <aimee/audit/obs_bus.h>
#include <aimee/delegates/module_api.h>
#include <aimee/git/module_api.h>
#include <aimee/learning/module_api.h>
#include <aimee/memory/module_api.h>
#include <aimee/response-composition/module_api.h>
#include <aimee/routing/module_api.h>
#include <aimee/runtime-web/module_api.h>
#include <aimee/skills/module_api.h>
#include <aimee/tools/module_api.h>
#include <aimee/workspace/module_api.h>
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
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
   dep_state_t retrieval;
   dep_state_t modules;
   char failed_boundary[32];
   char missing_module[32];
   char breaker_state[16];
   long long retry_after_ms;
   long long last_success_query_ms;
   char last_ingest_at[64];
   long sampled_at; /* epoch seconds; 0 = never sampled */
} ready_snapshot_t;

static pthread_mutex_t g_ready_mtx = PTHREAD_MUTEX_INITIALIZER;
static ready_snapshot_t g_snap; /* guarded by g_ready_mtx */
static pthread_t g_ready_thread;
static int g_ready_thread_started;

#define READY_INTERVAL_DEFAULT 15
#define READY_STALE_DEFAULT    60

/* Parse a positive integer env override. Malformed input falls back to the
 * documented default and says so, rather than silently becoming zero (which is
 * what atoi() would do) — a mistyped interval should be visible, not turn into
 * a different policy nobody chose. */
static int env_int(const char *name, int dflt, int min, int max)
{
   const char *v = getenv(name);
   if (!v || !v[0])
      return dflt;

   errno = 0;
   char *end = NULL;
   long n = strtol(v, &end, 10);
   if (errno != 0 || !end || *end != '\0' || n < (long)min || n > (long)max)
   {
      LOG_ERROR("ready", "%s=\"%s\" is not an integer in [%d,%d]; using %d", name, v, min, max,
                dflt);
      return dflt;
   }
   return (int)n;
}

static int ready_interval_secs(void)
{
   return env_int("AIMEE_READY_INTERVAL_SECS", READY_INTERVAL_DEFAULT, 1, 3600);
}

static int ready_stale_secs(void)
{
   return env_int("AIMEE_READY_STALE_SECS", READY_STALE_DEFAULT, ready_interval_secs(), 86400);
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

static void json_escape(const char *src, char *dst, size_t cap)
{
   size_t used = 0;
   if (cap == 0)
      return;
   dst[0] = '\0';
   if (!src)
      return;
   for (const unsigned char *p = (const unsigned char *)src; *p && used + 1 < cap; p++)
   {
      const char *escape = NULL;
      char unicode[7];
      if (*p == '"')
         escape = "\\\"";
      else if (*p == '\\')
         escape = "\\\\";
      else if (*p < 0x20)
      {
         snprintf(unicode, sizeof(unicode), "\\u%04x", *p);
         escape = unicode;
      }
      if (escape)
      {
         size_t n = strlen(escape);
         if (used + n >= cap)
            break;
         memcpy(dst + used, escape, n);
         used += n;
      }
      else
         dst[used++] = (char)*p;
   }
   dst[used] = '\0';
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
   kb_client_dependency_health_t dependency;
   kb_client_dependency_health(&dependency);
   snprintf(s.breaker_state, sizeof(s.breaker_state), "%s", dependency.state);
   s.retry_after_ms = dependency.retry_after_ms;
   s.last_success_query_ms = dependency.last_success_ms;
   snprintf(s.last_ingest_at, sizeof(s.last_ingest_at), "%s", h.last_ingest_at);
   s.retrieval = (s.kb == DEP_OK && h.db2_ok && h.db2_kb_tables_ok && h.pgvec_ok &&
                  h.pgvec_collection_ok && h.embed_ok && strcmp(dependency.state, "open") != 0)
                     ? DEP_OK
                     : DEP_FAIL;
   const char *failed = s.kb != DEP_OK                          ? "kb_transport"
                        : !h.db2_ok                             ? "db2"
                        : !h.db2_kb_tables_ok                   ? "kb_schema"
                        : !h.pgvec_ok                           ? "pgvector"
                        : !h.pgvec_collection_ok                ? "vector_collection"
                        : !h.embed_ok                           ? "embedder"
                        : strcmp(dependency.state, "open") == 0 ? "kb_breaker"
                                                                : "";
   snprintf(s.failed_boundary, sizeof(s.failed_boundary), "%s", failed);

   static const struct
   {
      uint32_t kind;
      const char *name;
   } required_modules[] = {
       {AIMEE_MEMORY_EVENT_RERANK, "memory"},
       {AIMEE_LEARNING_EVENT_OBSERVE, "learning"},
       {AIMEE_ROUTING_EVENT_KIND, "routing"},
       {AIMEE_DELEGATES_EVENT_INVOKE, "delegates"},
       {AIMEE_TOOLS_EVENT_DISPATCH, "tools"},
       {AIMEE_WORKSPACE_EVENT_ACCESS, "workspace"},
       {AIMEE_GIT_EVENT_OPERATION, "git"},
       {AIMEE_GIT_EVENT_REF_VALIDATE, "git"},
       {AIMEE_SKILLS_EVENT_CONTEXT, "skills"},
       {AIMEE_SKILLS_EVENT_TRIGGER, "skills"},
       {AIMEE_RESPONSE_EVENT_COMPOSE, "response-composition"},
       {AIMEE_RUNTIME_WEB_EVENT_CLASSIFY, "runtime-web"},
   };
   s.modules = DEP_OK;
   for (size_t i = 0; i < sizeof(required_modules) / sizeof(required_modules[0]); ++i)
   {
      if (obs_bus_module_available(required_modules[i].kind))
         continue;
      s.modules = DEP_FAIL;
      snprintf(s.missing_module, sizeof(s.missing_module), "%s", required_modules[i].name);
      break;
   }

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
int server_ready_render(int db1_ok, int kb_ok, const server_ready_diagnostics_t *diagnostics,
                        long sampled_at, long now, int stale_secs, char *resp, int cap)
{
   dep_state_t db1 = (db1_ok > 0) ? DEP_OK : (db1_ok == 0 ? DEP_FAIL : DEP_UNKNOWN);
   dep_state_t kb = (kb_ok > 0) ? DEP_OK : (kb_ok == 0 ? DEP_FAIL : DEP_UNKNOWN);
   int retrieval_ok = diagnostics ? diagnostics->retrieval_ok : -1;
   dep_state_t retrieval =
       (retrieval_ok > 0) ? DEP_OK : (retrieval_ok == 0 ? DEP_FAIL : DEP_UNKNOWN);
   int modules_ok = diagnostics ? diagnostics->modules_ok : -1;
   dep_state_t modules = (modules_ok > 0) ? DEP_OK : (modules_ok == 0 ? DEP_FAIL : DEP_UNKNOWN);

   long age = (sampled_at > 0) ? (now - sampled_at) : -1;

   /* Never sampled, or too old to trust — including a snapshot stamped in the
    * future, which means the clock moved and the age is meaningless. Degrade to
    * unknown so a wedged sampler cannot keep advertising a stale "ready". */
   int stale = (sampled_at <= 0) || (age < 0) || (age > (long)stale_secs);
   if (stale)
   {
      db1 = DEP_UNKNOWN;
      kb = DEP_UNKNOWN;
      retrieval = DEP_UNKNOWN;
      modules = DEP_UNKNOWN;
   }

   int ready = (db1 == DEP_OK && kb == DEP_OK && retrieval == DEP_OK && modules == DEP_OK);
   const char *status = ready ? "ok" : (stale ? "unknown" : "degraded");

   if (stale && (sampled_at <= 0 || age < 0))
      snprintf(resp, (size_t)cap,
               "{\"ready\":false,\"status\":\"unknown\",\"service\":\"aimee-server\","
               "\"sampled_at\":null,\"age_seconds\":null,"
               "\"dependencies\":{\"db1\":\"unknown\",\"kb\":\"unknown\","
               "\"retrieval\":\"unknown\",\"modules\":\"unknown\"},"
               "\"diagnostics\":{\"breaker_state\":\"unknown\","
               "\"failed_boundary\":\"unknown\",\"retry_after_ms\":0,"
               "\"last_success_query_ms\":0,\"last_ingest_at\":\"\","
               "\"missing_module\":\"unknown\"}}");
   else
   {
      char escaped_boundary[64];
      char escaped_breaker[64];
      char escaped_ingest[256];
      char escaped_module[64];
      json_escape(diagnostics ? diagnostics->failed_boundary : NULL, escaped_boundary,
                  sizeof(escaped_boundary));
      json_escape(diagnostics ? diagnostics->breaker_state : NULL, escaped_breaker,
                  sizeof(escaped_breaker));
      json_escape(diagnostics ? diagnostics->last_ingest_at : NULL, escaped_ingest,
                  sizeof(escaped_ingest));
      json_escape(diagnostics ? diagnostics->missing_module : NULL, escaped_module,
                  sizeof(escaped_module));
      snprintf(resp, (size_t)cap,
               "{\"ready\":%s,\"status\":\"%s\",\"service\":\"aimee-server\","
               "\"sampled_at\":%ld,\"age_seconds\":%ld,"
               "\"dependencies\":{\"db1\":\"%s\",\"kb\":\"%s\",\"retrieval\":\"%s\","
               "\"modules\":\"%s\"},"
               "\"diagnostics\":{\"failed_boundary\":\"%s\",\"breaker_state\":\"%s\","
               "\"retry_after_ms\":%lld,"
               "\"last_success_query_ms\":%lld,\"last_ingest_at\":\"%s\","
               "\"missing_module\":\"%s\"}}",
               ready ? "true" : "false", status, sampled_at, age, dep_name(db1), dep_name(kb),
               dep_name(retrieval), dep_name(modules), escaped_boundary, escaped_breaker,
               diagnostics ? diagnostics->retry_after_ms : 0,
               diagnostics ? diagnostics->last_success_query_ms : 0, escaped_ingest,
               escaped_module);
   }

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
   server_ready_diagnostics_t diagnostics = {
       .retrieval_ok = (s.retrieval == DEP_UNKNOWN) ? -1 : (s.retrieval == DEP_OK),
       .modules_ok = (s.modules == DEP_UNKNOWN) ? -1 : (s.modules == DEP_OK),
       .failed_boundary = s.failed_boundary,
       .missing_module = s.missing_module,
       .breaker_state = s.breaker_state,
       .retry_after_ms = s.retry_after_ms,
       .last_success_query_ms = s.last_success_query_ms,
       .last_ingest_at = s.last_ingest_at,
   };

   return server_ready_render(db1_ok, kb_ok, &diagnostics, s.sampled_at, (long)time(NULL),
                              ready_stale_secs(), resp, cap);
}

/* Start the sampler and register the provider. The first sample is taken by the
 * background thread, NOT here: kb_client_health() is an HTTP call, and doing it
 * synchronously on the startup path would let an unreachable or slow aimee-kb
 * delay — or wedge — server startup. That would contradict the whole point of
 * sampling off the request path. The cost is that readiness reads `unknown`
 * (503) until the first sample lands, which is the correct fail-closed answer
 * for a server that genuinely has not checked yet. */
void server_ready_register(void)
{
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
