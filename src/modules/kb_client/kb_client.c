#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_client.h"
#include <aimee/core/connection/auth.h>
#include <aimee/core/connection/endpoint.h>
#include "kb_client_cache.h"
#include "kb_client_internal.h"
#include "kb_client_mtls.h"
#include "agent_exec.h"
#include "cli_client.h"
#include "kb_paths.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "runtime_secret.h"
#include "dependency_breaker.h"
#include "log.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef AIMEE_POSIX
#include <poll.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef char *(*kb_client_error_json_fn)(const char *message);

/* Distributed-mode mTLS transport (kb_client_mtls.c) is linked into aimee and
 * aimee-server, where these are overridden by the real implementation. These
 * WEAK defaults make the dependency optional: a binary/test that links only
 * kb_client.o (no mTLS transport) simply sees "not configured" and falls through
 * to the HTTP-URL / Unix-socket transports — no extra link deps. */
__attribute__((weak)) int kb_client_mtls_configured(void)
{
   return 0;
}
__attribute__((weak)) char *kb_client_mtls_request(const char *method, const char *path,
                                                   const char *body, int *status_out)
{
   (void)method;
   (void)path;
   (void)body;
   if (status_out)
      *status_out = -1;
   return NULL;
}

/* One breaker per operation CLASS, not one for the whole dependency.
 *
 * Bulk ingestion and interactive reads have nothing to say about each other's
 * health: a slow or failing corpus ingest is not evidence that a symbol lookup
 * will fail, and sharing a failure budget meant one could suppress the other
 * process-wide. That is what happened -- ingest scans tripped the single
 * breaker and every unrelated KB call was refused with a 503 that never left
 * this process, while the KB was healthy and answering. */
static dependency_breaker_t g_kb_dependency[KB_DEP_CLASS_COUNT] = {DEPENDENCY_BREAKER_INITIALIZER,
                                                                   DEPENDENCY_BREAKER_INITIALIZER};
static int64_t (*g_kb_dependency_clock)(void);
#if defined(_MSC_VER)
static __declspec(thread) kb_client_result_status_t g_kb_last_result = KB_CLIENT_RESULT_UNAVAILABLE;
static __declspec(thread) char g_kb_last_dependency[32] = "kb";
static __declspec(thread) int64_t g_kb_last_observed_generation;
static __declspec(thread) int64_t g_kb_last_current_generation;
static __declspec(thread) int64_t g_kb_last_observed_dimension;
static __declspec(thread) int64_t g_kb_last_current_dimension;
static __declspec(thread) int64_t g_kb_last_retry_after_ms;
static __declspec(thread) int g_kb_last_suppressed;
#else
static _Thread_local kb_client_result_status_t g_kb_last_result = KB_CLIENT_RESULT_UNAVAILABLE;
static _Thread_local char g_kb_last_dependency[32] = "kb";
static _Thread_local int64_t g_kb_last_observed_generation;
static _Thread_local int64_t g_kb_last_current_generation;
static _Thread_local int64_t g_kb_last_observed_dimension;
static _Thread_local int64_t g_kb_last_current_dimension;
static _Thread_local int64_t g_kb_last_retry_after_ms;
/* Set when the local breaker refused the call, so the typed error can say the
 * KB was never contacted instead of blaming the operation the caller asked for. */
static _Thread_local int g_kb_last_suppressed;
#endif

static int64_t kb_dependency_now_ms(void)
{
   if (g_kb_dependency_clock)
      return g_kb_dependency_clock();
   return (int64_t)time(NULL) * 1000;
}

/* Which failure budget a path draws on. Bulk means work whose duration is a
 * property of the corpus rather than of the service being healthy: ingestion
 * and embedding. Everything else is interactive and must stay answerable while
 * bulk work is struggling. */
kb_dependency_class_t kb_dependency_class_for_path(const char *path)
{
   if (!path || !path[0])
      return KB_DEP_INTERACTIVE;
   static const char *bulk[] = {"/v1/code/scan", "/v1/code/build", "/v1/code/embed", "/v1/ingest",
                                "/v1/kb/build"};
   for (size_t i = 0; i < sizeof(bulk) / sizeof(bulk[0]); i++)
      if (strncmp(path, bulk[i], strlen(bulk[i])) == 0)
         return KB_DEP_BULK;
   return KB_DEP_INTERACTIVE;
}

kb_client_result_status_t kb_client_last_result_status(void)
{
   return g_kb_last_result;
}

const char *kb_client_result_status_name(kb_client_result_status_t status)
{
   switch (status)
   {
   case KB_CLIENT_RESULT_OK:
      return "ok";
   case KB_CLIENT_RESULT_EMPTY:
      return "empty";
   case KB_CLIENT_RESULT_ABSTAINED:
      return "abstained";
   case KB_CLIENT_RESULT_STALE:
      return "stale";
   case KB_CLIENT_RESULT_UNAUTHORIZED:
      return "unauthorized";
   default:
      return "unavailable";
   }
}

int kb_client_result_status_retryable(kb_client_result_status_t status)
{
   return status == KB_CLIENT_RESULT_UNAVAILABLE;
}

void kb_client_dependency_health(kb_client_dependency_health_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   dependency_breaker_snapshot_t snap;
   /* Interactive is what an operator means by "is the KB answering". A bulk
    * ingest budget that is open does not make the service unreachable, and
    * reporting it here is what made status contradict the next command. */
   dependency_breaker_snapshot(&g_kb_dependency[KB_DEP_INTERACTIVE], kb_dependency_now_ms(), &snap);
   const char *state =
       !snap.open ? "closed"
                  : (snap.probe_inflight || snap.retry_after_ms == 0 ? "half_open" : "open");
   snprintf(out->state, sizeof(out->state), "%s", state);
   out->failure_streak = snap.failure_streak;
   out->recovery_attempt = snap.open_count;
   out->retry_after_ms = snap.retry_after_ms;
   out->last_success_ms = snap.last_success_ms;
   out->last_failure_ms = snap.last_failure_ms;
   out->suppressed_calls = snap.suppressed_calls;
}

void kb_client_dependency_reset_for_tests(void)
{
   for (int i = 0; i < KB_DEP_CLASS_COUNT; i++)
      dependency_breaker_reset(&g_kb_dependency[i]);
   g_kb_last_result = KB_CLIENT_RESULT_UNAVAILABLE;
   snprintf(g_kb_last_dependency, sizeof(g_kb_last_dependency), "%s", "kb");
   g_kb_last_observed_generation = 0;
   g_kb_last_current_generation = 0;
   g_kb_last_observed_dimension = 0;
   g_kb_last_current_dimension = 0;
   g_kb_last_retry_after_ms = 0;
}

void kb_client_dependency_set_clock_for_tests(int64_t (*now_ms)(void))
{
   g_kb_dependency_clock = now_ms;
}

static int kb_json_primary_empty(const cJSON *root, const char *path)
{
   const char *field = NULL;
   if (path && strcmp(path, "/v1/search") == 0)
      field = "result";
   else if (path && (strstr(path, "/v1/code/find?") || strstr(path, "/v1/code/callers?") ||
                     strstr(path, "/v1/code/search?")))
      field = "hits";
   else if (path && strstr(path, "/v1/code/structure?"))
      field = "definitions";
   else if (path && strstr(path, "/v1/code/context?"))
      field = "results";
   else if (path && strstr(path, "/v1/code/projects?"))
      field = "projects";
   if (field)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, field);
      return (cJSON_IsArray(value) && cJSON_GetArraySize(value) == 0) ||
             (cJSON_IsString(value) && !value->valuestring[0]);
   }

   /* RPC actions share one transport path, so classify only known top-level
    * primary result fields. Auxiliary arrays (warnings, citations, trace data)
    * are deliberately absent: an empty auxiliary list cannot make a successful
    * answer empty. */
   static const char *const primary_fields[] = {
       "facts",        "memories",    "results",    "relations", "edges",
       "prospectives", "directives",  "provenance", "history",   "items",
       "episodes",     "definitions", "hits",       "projects",  NULL,
   };
   int saw_primary_array = 0;
   for (int i = 0; primary_fields[i]; i++)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, primary_fields[i]);
      if (cJSON_IsArray(value))
      {
         saw_primary_array = 1;
         if (cJSON_GetArraySize(value) > 0)
            return 0;
      }
   }
   if (saw_primary_array)
      return 1;
   const cJSON *facts = cJSON_GetObjectItemCaseSensitive(root, "facts");
   return cJSON_IsString(facts) && !facts->valuestring[0];
}

static kb_client_result_status_t kb_classify_json_result(const char *path, const char *json,
                                                         int *valid_out)
{
   if (valid_out)
      *valid_out = 0;
   cJSON *root = json ? cJSON_Parse(json) : NULL;
   if (!root)
      return KB_CLIENT_RESULT_UNAVAILABLE;
   if (valid_out)
      *valid_out = 1;
   if (cJSON_IsArray(root))
   {
      kb_client_result_status_t result =
          cJSON_GetArraySize(root) == 0 ? KB_CLIENT_RESULT_EMPTY : KB_CLIENT_RESULT_OK;
      cJSON_Delete(root);
      return result;
   }

   const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
   const cJSON *summary = cJSON_GetObjectItemCaseSensitive(root, "summary_status");
   const cJSON *freshness = cJSON_GetObjectItemCaseSensitive(root, "freshness");
   const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
   const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
   const cJSON *no_answer = cJSON_GetObjectItemCaseSensitive(root, "no_answer");
   const char *name = cJSON_IsString(status) ? status->valuestring : NULL;
   if (cJSON_IsString(summary) && strcmp(summary->valuestring, "unavailable") == 0)
      name = "unavailable";
   if (cJSON_IsObject(error))
   {
      const cJSON *type = cJSON_GetObjectItemCaseSensitive(error, "type");
      if (cJSON_IsString(type) && strcmp(type->valuestring, "project_not_current") == 0)
         name = "stale";
   }

   kb_client_result_status_t result = KB_CLIENT_RESULT_OK;
   if ((cJSON_IsBool(no_answer) && cJSON_IsTrue(no_answer)) ||
       (name && (strcmp(name, "no_answer") == 0 || strcmp(name, "abstained") == 0)))
      result = KB_CLIENT_RESULT_ABSTAINED;
   else if ((name && (strcmp(name, "empty") == 0 || strcmp(name, "not_found") == 0)) ||
            (name && strcmp(name, "error") == 0 && cJSON_IsString(message) &&
             strstr(message->valuestring, "not found")))
      result = KB_CLIENT_RESULT_EMPTY;
   else if (name && strcmp(name, "stale") == 0)
      result = KB_CLIENT_RESULT_STALE;
   else if (name && strcmp(name, "unauthorized") == 0)
      result = KB_CLIENT_RESULT_UNAUTHORIZED;
   else if (name && (strcmp(name, "unavailable") == 0 || strcmp(name, "error") == 0))
      result = KB_CLIENT_RESULT_UNAVAILABLE;
   else if (cJSON_IsString(freshness) && strcmp(freshness->valuestring, "current") != 0)
      result = KB_CLIENT_RESULT_STALE;
   else if (kb_json_primary_empty(root, path))
      result = KB_CLIENT_RESULT_EMPTY;
   cJSON_Delete(root);
   return result;
}

static int kb_transport_begin(const char *path, int *status_out)
{
   if (status_out)
      *status_out = -1;
   g_kb_last_result = KB_CLIENT_RESULT_UNAVAILABLE;
   snprintf(g_kb_last_dependency, sizeof(g_kb_last_dependency), "%s", "kb");
   g_kb_last_observed_generation = 0;
   g_kb_last_current_generation = 0;
   g_kb_last_observed_dimension = 0;
   g_kb_last_current_dimension = 0;
   g_kb_last_retry_after_ms = 0;
   g_kb_last_suppressed = 0;
   int64_t now_ms = kb_dependency_now_ms();
   int64_t retry_after = 0;
   kb_dependency_class_t cls = kb_dependency_class_for_path(path);
   if (dependency_breaker_allow(&g_kb_dependency[cls], now_ms, &retry_after))
      return 1;

   /* Carry the breaker's own retry window into the typed error. Discarding it
    * made every suppressed call report the synthetic base-delay floor that
    * kb_typed_error_json() substitutes for a missing hint, so an operator could
    * not tell a one-second wait from a thirty-second one — or even tell that the
    * breaker, rather than the KB, produced the refusal. */
   g_kb_last_retry_after_ms = retry_after;
   g_kb_last_suppressed = 1;
   if (status_out)
      *status_out = 503;
   return 0;
}

static void kb_capture_result_metadata(const char *response)
{
   cJSON *root = response ? cJSON_Parse(response) : NULL;
   if (!root)
      return;
   const cJSON *dependency = cJSON_GetObjectItemCaseSensitive(root, "dependency");
   const cJSON *observed = cJSON_GetObjectItemCaseSensitive(root, "observed_generation");
   const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current_generation");
   const cJSON *observed_dimension = cJSON_GetObjectItemCaseSensitive(root, "observed_dimension");
   const cJSON *current_dimension = cJSON_GetObjectItemCaseSensitive(root, "current_dimension");
   const cJSON *retry_after = cJSON_GetObjectItemCaseSensitive(root, "retry_after_ms");
   if (cJSON_IsString(dependency) && dependency->valuestring[0])
      snprintf(g_kb_last_dependency, sizeof(g_kb_last_dependency), "%s", dependency->valuestring);
   if (cJSON_IsNumber(observed))
      g_kb_last_observed_generation = (int64_t)observed->valuedouble;
   if (cJSON_IsNumber(current))
      g_kb_last_current_generation = (int64_t)current->valuedouble;
   if (cJSON_IsNumber(observed_dimension))
      g_kb_last_observed_dimension = (int64_t)observed_dimension->valuedouble;
   if (cJSON_IsNumber(current_dimension))
      g_kb_last_current_dimension = (int64_t)current_dimension->valuedouble;
   if (cJSON_IsNumber(retry_after) && retry_after->valuedouble > 0)
      g_kb_last_retry_after_ms = (int64_t)retry_after->valuedouble;
   cJSON_Delete(root);
}

/* Report success, and log the close so an outage has a visible end as well as a
 * visible start. Silent recovery left an operator unable to tell a healed
 * dependency from one that simply had not been called again. */
static void kb_transport_recovered_class(kb_dependency_class_t cls, int64_t now_ms)
{
   dependency_breaker_snapshot_t before;
   dependency_breaker_snapshot(&g_kb_dependency[cls], now_ms, &before);
   dependency_breaker_report_success(&g_kb_dependency[cls], now_ms);
   if (before.open)
      LOG_INFO("server.kb", "knowledge service reachable again after %u open interval(s)",
               before.open_count);
}

/* A call that spent its whole budget and got nothing is a TIMEOUT, not evidence
 * the dependency is down. The transport cannot tell them apart -- a read timeout
 * and a refused connection both surface as status -1 with no body -- but the
 * caller knows what it allowed, so elapsed time separates them: failing fast
 * means nobody answered, failing at the deadline means someone is still working.
 *
 * This mattered because a slow ingest scan was recorded as an outage, three of
 * them opened the shared breaker, and every unrelated KB call was then
 * suppressed with a 503 that never left the process. */
int kb_transport_call_timed_out(int http_status, const char *response, int64_t elapsed_ms,
                                int timeout_ms)
{
   if (response || http_status > 0 || timeout_ms <= 0)
      return 0;
   return elapsed_ms >= (int64_t)timeout_ms * 9 / 10;
}

static void kb_transport_complete_timed(const char *path, const char *response, int http_status,
                                        int64_t elapsed_ms, int timeout_ms);

static void kb_transport_complete(const char *path, const char *response, int http_status)
{
   kb_transport_complete_timed(path, response, http_status, 0, 0);
}

static void kb_transport_complete_timed(const char *path, const char *response, int http_status,
                                        int64_t elapsed_ms, int timeout_ms)
{
   int64_t now_ms = kb_dependency_now_ms();
   int valid = 0;
   kb_client_result_status_t classified = kb_classify_json_result(path, response, &valid);
   if (valid)
      kb_capture_result_metadata(response);
   if (http_status == 401 || http_status == 403)
   {
      kb_transport_recovered_class(kb_dependency_class_for_path(path), now_ms);
      g_kb_last_result = KB_CLIENT_RESULT_UNAUTHORIZED;
      return;
   }
   if (valid && classified == KB_CLIENT_RESULT_UNAVAILABLE &&
       strcmp(g_kb_last_dependency, "kb") != 0)
   {
      /* The KB is reachable and truthfully reported one of its own optional
       * dependencies. Keep that typed outage from suppressing unrelated KB
       * operations through the transport breaker. */
      kb_transport_recovered_class(kb_dependency_class_for_path(path), now_ms);
      g_kb_last_result = classified;
      return;
   }
   if (http_status >= 200 && http_status < 300 && response && valid)
   {
      /* A typed unavailable body means the KB answered but one of its own
       * dependencies did not; do not trip the KB transport breaker. */
      kb_transport_recovered_class(kb_dependency_class_for_path(path), now_ms);
      g_kb_last_result = classified;
      return;
   }
   if (http_status >= 400 && http_status < 500 && http_status != 408 && http_status != 425 &&
       http_status != 429)
   {
      kb_transport_recovered_class(kb_dependency_class_for_path(path), now_ms);
      g_kb_last_result = valid ? classified : KB_CLIENT_RESULT_UNAVAILABLE;
      return;
   }
   /* This is the only branch that means the KB itself did not answer, and it was
    * silent. A server that cannot open its mTLS connection reported exactly the
    * same "<operation> unavailable" text as a healthy KB with an empty index, so
    * nothing in the log distinguished a transport outage from a data problem.
    * Log the transition into the open state — not every suppressed call — so a
    * sustained outage costs one line per backoff window rather than one per
    * request. */
   if (kb_transport_call_timed_out(http_status, response, elapsed_ms, timeout_ms))
   {
      /* The caller's budget ran out. The KB may well still be working on it, so
       * this says nothing about reachability and must not open the breaker. */
      g_kb_last_result = KB_CLIENT_RESULT_UNAVAILABLE;
      return;
   }
   kb_dependency_class_t fail_cls = kb_dependency_class_for_path(path);
   dependency_breaker_snapshot_t before;
   dependency_breaker_snapshot(&g_kb_dependency[fail_cls], now_ms, &before);
   dependency_breaker_report_failure(
       &g_kb_dependency[fail_cls], now_ms, DEPENDENCY_BREAKER_DEFAULT_THRESHOLD,
       DEPENDENCY_BREAKER_DEFAULT_BASE_MS, DEPENDENCY_BREAKER_DEFAULT_MAX_MS);
   dependency_breaker_snapshot_t after;
   dependency_breaker_snapshot(&g_kb_dependency[fail_cls], now_ms, &after);
   if (after.open && !before.open)
      LOG_WARN("server.kb",
               "knowledge service unreachable (%s %s); suppressing calls for %lldms after %u "
               "consecutive failure(s)",
               http_status > 0 ? "http" : "transport", path && path[0] ? path : "(no path)",
               (long long)after.retry_after_ms, after.failure_streak);
   g_kb_last_result = KB_CLIENT_RESULT_UNAVAILABLE;
}

static char *kb_typed_error_json(kb_client_result_status_t status, const char *message)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return strdup("{\"status\":\"unavailable\"}");
   cJSON_AddStringToObject(obj, "status", kb_client_result_status_name(status));
   cJSON_AddBoolToObject(obj, "retryable", kb_client_result_status_retryable(status));
   if (status == KB_CLIENT_RESULT_UNAVAILABLE || status == KB_CLIENT_RESULT_STALE)
      cJSON_AddStringToObject(obj, "dependency",
                              g_kb_last_dependency[0] ? g_kb_last_dependency : "kb");
   if (status == KB_CLIENT_RESULT_UNAVAILABLE)
   {
      kb_client_dependency_health_t health;
      kb_client_dependency_health(&health);
      int64_t retry_after_ms =
          g_kb_last_retry_after_ms > 0 ? g_kb_last_retry_after_ms : health.retry_after_ms;
      if (retry_after_ms <= 0)
         retry_after_ms = DEPENDENCY_BREAKER_DEFAULT_BASE_MS;
      if (retry_after_ms > DEPENDENCY_BREAKER_DEFAULT_MAX_MS)
         retry_after_ms = DEPENDENCY_BREAKER_DEFAULT_MAX_MS;
      cJSON_AddNumberToObject(obj, "retry_after_ms", (double)retry_after_ms);
      /* Distinguish "we never called the KB" from "the KB answered unavailable".
       * Both used to surface as the caller's own operation being unavailable,
       * which sends an operator to the index when the fault is local. */
      if (g_kb_last_suppressed)
         cJSON_AddBoolToObject(obj, "circuit_open", 1);
   }
   if (status == KB_CLIENT_RESULT_STALE && g_kb_last_observed_generation > 0)
      cJSON_AddNumberToObject(obj, "observed_generation", (double)g_kb_last_observed_generation);
   if (status == KB_CLIENT_RESULT_STALE && g_kb_last_current_generation > 0)
      cJSON_AddNumberToObject(obj, "current_generation", (double)g_kb_last_current_generation);
   if (status == KB_CLIENT_RESULT_STALE && g_kb_last_observed_dimension > 0)
      cJSON_AddNumberToObject(obj, "observed_dimension", (double)g_kb_last_observed_dimension);
   if (status == KB_CLIENT_RESULT_STALE && g_kb_last_current_dimension > 0)
      cJSON_AddNumberToObject(obj, "current_dimension", (double)g_kb_last_current_dimension);
   cJSON_AddStringToObject(obj, "message", message ? message : "knowledge service unavailable");
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{\"status\":\"unavailable\"}");
}

char *kb_client_last_result_json(const char *message)
{
   return kb_typed_error_json(kb_client_last_result_status(), message);
}

static char *kb_v1_action_request_timeout(const char *action, cJSON *req, int timeout_ms,
                                          kb_client_error_json_fn error_json);

char *kb_client_query_escape(const char *s)
{
   if (!s)
      s = "";
   size_t len = strlen(s);
   char *out = malloc(len * 3 + 1);
   if (!out)
      return NULL;
   size_t j = 0;
   for (size_t i = 0; i < len; i++)
   {
      unsigned char c = (unsigned char)s[i];
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      {
         out[j++] = (char)c;
      }
      else
      {
         out[j++] = '%';
         out[j++] = "0123456789ABCDEF"[(c >> 4) & 0x0F];
         out[j++] = "0123456789ABCDEF"[c & 0x0F];
      }
   }
   out[j] = '\0';
   return out;
}

static char *kb_status_unavailable_json(const char *message)
{
   char *json = kb_typed_error_json(KB_CLIENT_RESULT_UNAVAILABLE, message);
   cJSON *obj = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!obj)
      return strdup("{\"status\":\"unavailable\"}");
   cJSON_AddStringToObject(obj, "summary_status", "unavailable");
   cJSON_AddStringToObject(obj, "owner", "knowledge-service");
   cJSON_AddBoolToObject(obj, "available", 0);
   json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{\"status\":\"unavailable\"}");
}

static char *kb_vector_unavailable_json(const char *message)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return strdup("{}");
   cJSON_AddStringToObject(obj, "status", "unavailable");
   cJSON_AddStringToObject(obj, "owner", "knowledge-service");
   cJSON_AddBoolToObject(obj, "available", 0);
   cJSON_AddStringToObject(obj, "message", message ? message : "knowledge service unavailable");
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{}");
}

int kb_client_is_live(void)
{
   /* A configured remote endpoint owns its own reachability/timeout, so treat
    * it as "live" and let the actual call apply its timeout rather than probing
    * twice here. */
   kb_client_dependency_health_t health;
   kb_client_dependency_health(&health);
   return (kb_client_v1_base_url() != NULL || kb_client_mtls_configured()) &&
          strcmp(health.state, "open") != 0;
}

char *kb_client_health_json(void)
{
   int http_status = -1;
   char *body = kb_client_v1_get_json("/v1/health", CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (!body)
      return NULL;
   if (http_status < 200 || http_status >= 300)
   {
      free(body);
      return NULL;
   }
   return body;
}

/* Cached read of the KB's advertised typed-facts capability (proposal §8).
 * aimee-server gates per-turn fact injection on THIS instead of owning
 * typed_facts_enabled itself — the KB is the single source of truth. Short TTL so
 * a facts-off deployment does not fetch /v1/health every turn; a console/config
 * change is picked up within the TTL. Benign cross-thread race on the cache (worst
 * case: a couple of extra fetches). On transport failure the last-known value (or
 * off) is kept, so a briefly-unreachable KB never spuriously injects. */
int kb_client_typed_facts_enabled(void)
{
   static int cached = -1; /* -1 = never fetched */
   static time_t fetched_at = 0;
   const int TTL_S = 15;
   time_t now = time(NULL);
   if (cached >= 0 && (now - fetched_at) < TTL_S)
      return cached;
   int v = cached >= 0 ? cached : 0;
   char *h = kb_client_health_json();
   if (h)
   {
      cJSON *j = cJSON_Parse(h);
      free(h);
      if (j)
      {
         v = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "typed_facts_enabled")) ? 1 : 0;
         cJSON_Delete(j);
      }
   }
   cached = v;
   fetched_at = now;
   return v;
}

/* §2c: POST /v1/reembed {confirm, force, dry_run} — the dim-change reset. Returns
 * the raw response JSON (caller frees), or NULL on transport failure; *status_out
 * (optional) gets the HTTP status. A long timeout covers the DROP + schema re-apply. */
char *kb_client_reembed(int confirm, int force, int dry_run, int target_dim, int clear_maintenance,
                        int *status_out)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return NULL;
   if (clear_maintenance)
      cJSON_AddBoolToObject(req, "clear_maintenance", 1);
   cJSON_AddBoolToObject(req, "confirm", confirm ? 1 : 0);
   cJSON_AddBoolToObject(req, "force", force ? 1 : 0);
   cJSON_AddBoolToObject(req, "dry_run", dry_run ? 1 : 0);
   if (target_dim > 0)
      cJSON_AddNumberToObject(req, "target_dim", target_dim);
   char *resp = kb_client_v1_post_json_keep_error("/v1/reembed", req, 120000, status_out);
   cJSON_Delete(req);
   return resp;
}

/* The curator observability block from aimee-kb's /v1/health (§4), returned as a
 * standalone JSON object for the server's GET /v1/kb/curator surface. Never
 * returns NULL: an unavailable kb / missing block yields a status-error object. */
char *kb_client_curator_json(void)
{
   char *health = kb_client_health_json();
   if (!health)
      return kb_status_unavailable_json("knowledge service /v1/health did not respond");
   cJSON *root = cJSON_Parse(health);
   free(health);
   cJSON *cur = root ? cJSON_DetachItemFromObjectCaseSensitive(root, "curator") : NULL;
   cJSON_Delete(root);
   if (!cur)
      return kb_status_unavailable_json("curator status unavailable");
   char *out = cJSON_PrintUnformatted(cur);
   cJSON_Delete(cur);
   return out ? out : kb_status_unavailable_json("curator status serialization failed");
}

/* Flatten a JSON array of strings into a newline-separated buffer, truncating at
 * the buffer rather than overrunning it. Extracted when `blockers` joined
 * `warnings` and the second caller made a copy-paste of the pointer arithmetic
 * the obvious alternative. A non-array (including a missing key, which is how an
 * older kb answers) leaves the buffer untouched. */
static void kb_client_join_strings(cJSON *arr, char *buf, size_t cap)
{
   if (!cJSON_IsArray(arr) || !buf || cap == 0)
      return;
   size_t pos = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, arr)
   {
      if (!cJSON_IsString(item))
         continue;
      if (pos > 0 && pos < cap - 1)
         buf[pos++] = '\n';
      size_t rem = cap - pos - 1;
      if (rem == 0)
         break;
      size_t len = strlen(item->valuestring);
      if (len > rem)
         len = rem;
      memcpy(buf + pos, item->valuestring, len);
      pos += len;
      buf[pos] = '\0';
   }
}

int kb_client_health(kb_health_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->freshness_days = -1;

   int http_status = -1;
   char *body = kb_client_v1_get_json("/v1/health", CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (!body)
      return -1;
   cJSON *resp = cJSON_Parse(body);
   free(body);
   if (http_status < 200 || http_status >= 300 || !resp)
   {
      cJSON_Delete(resp);
      return -1;
   }
   if (!resp)
      return -1;

   /* process_ok means SOMETHING ANSWERED, which is the only thing this check can
    * honestly establish. It used to demand status == "ok" exactly and return -1
    * otherwise — so the moment the kb learned to say "degraded", a kb that was up
    * and telling us precisely what was wrong would have been reported to every
    * caller as unreachable, and its blockers discarded unread. The verdict is
    * carried in out->status for callers to act on; it is not this function's job
    * to turn a diagnosis into a transport failure.
    *
    * Any status string counts as an answer. An unparseable or non-200 response
    * has already returned -1 above, which is the real "did not answer". */
   cJSON *s = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(s))
   {
      cJSON_Delete(resp);
      return -1;
   }
   out->process_ok = 1;

#define COPY_BOOL(field, key)                                                                      \
   do                                                                                              \
   {                                                                                               \
      cJSON *_j = cJSON_GetObjectItemCaseSensitive(resp, key);                                     \
      if (cJSON_IsBool(_j))                                                                        \
         out->field = cJSON_IsTrue(_j) ? 1 : 0;                                                    \
   } while (0)
#define COPY_INT(field, key)                                                                       \
   do                                                                                              \
   {                                                                                               \
      cJSON *_j = cJSON_GetObjectItemCaseSensitive(resp, key);                                     \
      if (cJSON_IsNumber(_j))                                                                      \
         out->field = (int)_j->valuedouble;                                                        \
   } while (0)
#define COPY_STR(field, key)                                                                       \
   do                                                                                              \
   {                                                                                               \
      cJSON *_j = cJSON_GetObjectItemCaseSensitive(resp, key);                                     \
      if (cJSON_IsString(_j))                                                                      \
         snprintf(out->field, sizeof(out->field), "%s", _j->valuestring);                          \
   } while (0)

   COPY_BOOL(db2_ok, "db2_ok");
   COPY_BOOL(db2_kb_tables_ok, "db2_kb_tables_ok");
   COPY_BOOL(pgvec_ok, "pgvec_ok");
   COPY_BOOL(pgvec_collection_ok, "pgvec_collection_ok");
   COPY_INT(pgvec_vectors, "pgvec_vectors");
   COPY_INT(pgvec_indexed, "pgvec_indexed_vectors");
   COPY_BOOL(embed_ok, "embed_ok");
   COPY_STR(embed_command, "embed_command");
   COPY_STR(status, "status");
   COPY_INT(freshness_days, "freshness_days");
   COPY_STR(last_ingest_at, "last_ingest_at");
   COPY_INT(chunk_count, "chunk_count");
   COPY_INT(embedding_count, "embedding_count");

   /* maintenance fields */
   cJSON *lma = cJSON_GetObjectItemCaseSensitive(resp, "last_maintenance_at");
   if (cJSON_IsString(lma))
      snprintf(out->last_maintenance_at, sizeof(out->last_maintenance_at), "%s", lma->valuestring);

   cJSON *mdr = cJSON_GetObjectItemCaseSensitive(resp, "last_maintenance_rows_decayed");
   if (cJSON_IsNumber(mdr))
      out->last_maintenance_rows_decayed = (int)mdr->valuedouble;

   cJSON *mop = cJSON_GetObjectItemCaseSensitive(resp, "last_maintenance_orphans_pruned");
   if (cJSON_IsNumber(mop))
      out->last_maintenance_orphans_pruned = (int)mop->valuedouble;

   cJSON *men = cJSON_GetObjectItemCaseSensitive(resp, "maintenance_enabled");
   if (cJSON_IsBool(men))
      out->maintenance_enabled = cJSON_IsTrue(men) ? 1 : 0;

#undef COPY_BOOL
#undef COPY_INT
#undef COPY_STR

   kb_client_join_strings(cJSON_GetObjectItemCaseSensitive(resp, "warnings"), out->warnings,
                          sizeof(out->warnings));
   kb_client_join_strings(cJSON_GetObjectItemCaseSensitive(resp, "blockers"), out->blockers,
                          sizeof(out->blockers));

   cJSON_Delete(resp);

   /* Health proves that the dependency is reachable; version proves which
    * dependency answered. Keep this best-effort for compatibility with an
    * older KB, but surface it whenever /v1/version is available so operators
    * and benchmark harnesses can pin the complete distributed stack. */
   int version_status = -1;
   char *version_body =
       kb_client_v1_get_json("/v1/version", CLIENT_DEFAULT_TIMEOUT_MS, &version_status);
   cJSON *version_json = version_body ? cJSON_Parse(version_body) : NULL;
   free(version_body);
   cJSON *version = version_json ? cJSON_GetObjectItemCaseSensitive(version_json, "version") : NULL;
   cJSON *service = version_json ? cJSON_GetObjectItemCaseSensitive(version_json, "service") : NULL;
   if (version_status >= 200 && version_status < 300 && cJSON_IsString(version) &&
       version->valuestring[0] && cJSON_IsString(service) &&
       strcmp(service->valuestring, "aimee-kb") == 0)
      snprintf(out->version, sizeof(out->version), "%s", version->valuestring);
   cJSON_Delete(version_json);
   return 0;
}

static char *kb_client_status_request(const char *project)
{
   char path[512];
   if (project && project[0])
   {
      char *encoded_project = kb_client_query_escape(project);
      if (!encoded_project)
         return kb_status_unavailable_json(
             "out of memory building knowledge service status request");
      int n = snprintf(path, sizeof(path), "/v1/health?status=1&project=%s", encoded_project);
      free(encoded_project);
      if (n < 0 || (size_t)n >= sizeof(path))
         return kb_status_unavailable_json("knowledge service status project name is too long");
   }
   else
      snprintf(path, sizeof(path), "/v1/health?status=1");

   int http_status = -1;
   char *resp = kb_client_v1_get_json(path, CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/health status returned HTTP %d",
               http_status);
      return kb_status_unavailable_json(msg);
   }
   return kb_status_unavailable_json("knowledge service /v1/health status did not respond");
}

char *kb_client_status_json(void)
{
   return kb_client_status_request(NULL);
}

char *kb_client_project_status_json(const char *project)
{
   return kb_client_status_request(project);
}

/* Long enough for a full force-rebuild of a large repo (kb_build can take
 * minutes).  If the operator wants to bound this further they can interrupt
 * the CLI which closes the socket and the server-side repair will continue
 * to completion — same trade-off as before this RPC existed. */
#define KB_CLIENT_REPAIR_TIMEOUT_MS (10 * 60 * 1000)

static char *kb_error_json(const char *message)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return strdup("{\"status\":\"error\"}");
   cJSON_AddStringToObject(obj, "status", "error");
   cJSON_AddStringToObject(obj, "message", message ? message : "knowledge service unavailable");
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{\"status\":\"error\"}");
}

/* Synchronous build/update now run entirely inside aimee-kb (which owns DB2
 * and the filesystem) via the /v1/code/{build,update} endpoints — no
 * server-side compute or chunk push. */
static char *kb_client_code_post_json(const char *endpoint, const char *path, const char *project,
                                      const char *embedding_command, int force)
{
   if (!path || !path[0] || !project || !project[0])
      return kb_error_json("build/update requires path and project");

   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   cJSON_AddStringToObject(req, "path", path);
   cJSON_AddStringToObject(req, "project", project);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   if (force)
      cJSON_AddTrueToObject(req, "force");

   int http_status = -1;
   char *resp = kb_client_v1_post_json(endpoint, req, KB_CLIENT_REPAIR_TIMEOUT_MS, &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "knowledge service %s returned HTTP %d", endpoint, http_status);
      return kb_error_json(msg);
   }
   char msg[160];
   snprintf(msg, sizeof(msg), "knowledge service %s did not respond", endpoint);
   return kb_error_json(msg);
}

char *kb_client_build_json(const char *path, const char *project, const char *embedding_command,
                           int force)
{
   return kb_client_code_post_json("/v1/code/build", path, project, embedding_command,
                                   force ? 1 : 0);
}

char *kb_client_update_json(const char *path, const char *project, const char *embedding_command)
{
   return kb_client_code_post_json("/v1/code/update", path, project, embedding_command, 0);
}

char *kb_client_ingest_json(const char *workspace, const char *embedding_command, int force)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   if (workspace && workspace[0])
      cJSON_AddStringToObject(req, "workspace", workspace);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   if (force)
      cJSON_AddTrueToObject(req, "force");
   int http_status = -1;
   char *resp =
       kb_client_v1_post_json("/v1/ingest", req, KB_CLIENT_REPAIR_TIMEOUT_MS, &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/ingest returned HTTP %d", http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/ingest did not respond");
}

char *kb_client_ingest_status_json(void)
{
   int http_status = -1;
   char *resp = kb_client_v1_get_json("/v1/ingest/status", CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/ingest/status returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/ingest/status did not respond");
}

char *kb_client_queue_status_json(void)
{
   int http_status = -1;
   char *resp =
       kb_client_v1_get_json("/v1/pipeline/status", CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/pipeline/status returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/pipeline/status did not respond");
}

char *kb_client_corpus_pipeline_status_json(void)
{
   int http_status = -1;
   char *resp =
       kb_client_v1_get_json("/v1/corpus/pipeline/status", CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/corpus/pipeline/status returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/corpus/pipeline/status did not respond");
}

char *kb_client_job_status_json(int64_t job_id)
{
   if (job_id <= 0)
      return kb_error_json("invalid job id");
   char path[64];
   snprintf(path, sizeof(path), "/v1/jobs/%lld", (long long)job_id);
   int http_status = -1;
   char *resp = kb_client_v1_get_json(path, CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service %s returned HTTP %d", path, http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/jobs did not respond");
}

char *kb_client_queue_drain_json(const char *embedding_command, int timeout_secs)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   cJSON_AddNumberToObject(req, "timeout", timeout_secs);
   /* Drains block until the queue empties or timeout elapses; size the
    * RPC timeout a bit above the caller's timeout so the network wait
    * outlasts the drain.  Cap at 10 minutes so a wedged drain can't
    * hang the CLI indefinitely. */
   int req_timeout = (timeout_secs > 0 ? timeout_secs + 30 : 600) * 1000;
   if (req_timeout > 10 * 60 * 1000)
      req_timeout = 10 * 60 * 1000;
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/drain", req, req_timeout, &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/drain returned HTTP %d", http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/drain did not respond");
}

char *kb_client_corpus_pipeline_drain_json(int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   cJSON_AddNumberToObject(req, "limit", limit);
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/corpus/pipeline/drain", req, CLIENT_DEFAULT_TIMEOUT_MS,
                                       &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/corpus/pipeline/drain returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/corpus/pipeline/drain did not respond");
}

/* Reconcile scans DB2 and issues pgvector deletes; size the timeout generously
 * but bounded. */
#define KB_CLIENT_RECONCILE_TIMEOUT_MS (2 * 60 * 1000)

char *kb_client_reconcile_json(int dry_run)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return kb_error_json("out of memory");
   cJSON_AddBoolToObject(req, "dry_run", dry_run ? 1 : 0);
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/maintenance/reconcile", req,
                                       KB_CLIENT_RECONCILE_TIMEOUT_MS, &http_status);
   cJSON_Delete(req);
   if (resp)
      return resp;
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/maintenance/reconcile returned HTTP %d",
               http_status);
      return kb_error_json(msg);
   }
   return kb_error_json("knowledge service /v1/maintenance/reconcile did not respond");
}

/* memory.reindex scans the memory corpus and rebuilds derived tables; give
 * it a bounded but generous timeout. */
#define KB_CLIENT_MEMORY_REINDEX_TIMEOUT_MS (5 * 60 * 1000)

char *kb_client_memory_reindex_json(int limit)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request_timeout("memory.reindex", req, KB_CLIENT_MEMORY_REINDEX_TIMEOUT_MS,
                                       kb_error_json);
}

/* Memory rebuild re-upserts every memory point to pgvector; size the timeout
 * like the repair path. */
#define KB_CLIENT_MEMORY_REBUILD_TIMEOUT_MS (10 * 60 * 1000)

char *kb_client_memory_rebuild_json(const char *version)
{
   cJSON *req = cJSON_CreateObject();
   if (version && version[0])
      cJSON_AddStringToObject(req, "version", version);
   return kb_v1_action_request_timeout("memory.rebuild", req, KB_CLIENT_MEMORY_REBUILD_TIMEOUT_MS,
                                       kb_error_json);
}

#define KB_CLIENT_DIRECTIVE_TIMEOUT_MS (60 * 1000)

static char *kb_v1_action_request_timeout(const char *action, cJSON *req, int timeout_ms,
                                          kb_client_error_json_fn error_json)
{
   if (!action || !action[0])
   {
      cJSON_Delete(req);
      return error_json("missing knowledge service action");
   }
   if (!req)
      req = cJSON_CreateObject();
   if (!req)
      return error_json("failed to allocate knowledge service request");

   char *escaped = kb_client_query_escape(action);
   if (!escaped)
   {
      cJSON_Delete(req);
      return error_json("failed to encode knowledge service action");
   }

   char path[256];
   snprintf(path, sizeof(path), "/v1/actions/%s", escaped);
   free(escaped);

   int http_status = -1;
   /* keep_error: on a refusal the kb's body says WHY. Without it this returned a
    * synthesised "returned HTTP 400", which relays as a successful response
    * containing an error nobody reads -- the caller saw exit 0 and no output. */
   char *json = kb_client_v1_post_json_keep_error(path, req, timeout_ms, &http_status);
   cJSON_Delete(req);
   if (json)
      return json;

   if (http_status >= 100)
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "knowledge service action %s returned HTTP %d", action,
               http_status);
      return kb_typed_error_json(kb_client_last_result_status(), msg);
   }
   return kb_typed_error_json(kb_client_last_result_status(),
                              "knowledge service action did not respond");
}

/* Shared with kb_client_memory.c — keep external linkage. */
char *kb_v1_action_request(const char *action, cJSON *req)
{
   return kb_v1_action_request_timeout(action, req, KB_CLIENT_DIRECTIVE_TIMEOUT_MS, kb_error_json);
}

char *kb_client_memory_directive_create_json(const char *question, const char *topic,
                                             const char *entity, const char *file,
                                             const char *cause, int priority, const char *session,
                                             const char *valid_until)
{
   if (!question || !question[0])
      return kb_error_json("memory.directive_create requires question");
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "question", question);
   if (topic && topic[0])
      cJSON_AddStringToObject(req, "topic", topic);
   if (entity && entity[0])
      cJSON_AddStringToObject(req, "entity", entity);
   if (file && file[0])
      cJSON_AddStringToObject(req, "file", file);
   if (cause && cause[0])
      cJSON_AddStringToObject(req, "cause", cause);
   cJSON_AddNumberToObject(req, "priority", priority);
   if (session && session[0])
      cJSON_AddStringToObject(req, "session", session);
   if (valid_until && valid_until[0])
      cJSON_AddStringToObject(req, "valid_until", valid_until);
   return kb_v1_action_request("memory.directive_create", req);
}

char *kb_client_memory_directive_resolve_json(int64_t id, int64_t with_memory, const char *note)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   if (with_memory > 0)
      cJSON_AddNumberToObject(req, "with_memory", (double)with_memory);
   if (note && note[0])
      cJSON_AddStringToObject(req, "note", note);
   return kb_v1_action_request("memory.directive_resolve", req);
}

char *kb_client_memory_directive_suppress_json(int64_t id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   return kb_v1_action_request("memory.directive_suppress", req);
}

char *kb_client_memory_directive_sweep_expired_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("memory.directive_sweep_expired", req);
}

char *kb_client_memory_directive_list_json(const char *state, const char *cause, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (state && state[0])
      cJSON_AddStringToObject(req, "state", state);
   if (cause && cause[0])
      cJSON_AddStringToObject(req, "cause", cause);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("memory.directive_list", req);
}

char *kb_client_curiosity_list_json(const char *state, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (state && state[0])
      cJSON_AddStringToObject(req, "state", state);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("curiosity.list", req);
}

char *kb_client_curiosity_create_json(const char *gap_type, const char *target_entity,
                                      const char *target_topic, const char *evidence,
                                      double importance, double novelty, const char *source_session)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "gap_type", gap_type ? gap_type : "");
   if (target_entity && target_entity[0])
      cJSON_AddStringToObject(req, "target_entity", target_entity);
   if (target_topic && target_topic[0])
      cJSON_AddStringToObject(req, "target_topic", target_topic);
   if (evidence && evidence[0])
      cJSON_AddStringToObject(req, "evidence", evidence);
   cJSON_AddNumberToObject(req, "importance", importance);
   cJSON_AddNumberToObject(req, "novelty", novelty);
   if (source_session && source_session[0])
      cJSON_AddStringToObject(req, "source_session", source_session);
   return kb_v1_action_request("curiosity.create", req);
}

char *kb_client_curiosity_sweep_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("curiosity.sweep", req);
}

char *kb_client_curiosity_rescore_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("curiosity.rescore", req);
}

char *kb_client_curiosity_get_json(int64_t id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   return kb_v1_action_request("curiosity.get", req);
}

char *kb_client_curiosity_update_state_json(int64_t id, const char *new_state)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   cJSON_AddStringToObject(req, "state", new_state ? new_state : "");
   return kb_v1_action_request("curiosity.update_state", req);
}

char *kb_client_curiosity_route_top_json(int limit, const char *source_session)
{
   cJSON *req = cJSON_CreateObject();
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   if (source_session && source_session[0])
      cJSON_AddStringToObject(req, "source_session", source_session);
   return kb_v1_action_request("curiosity.route_top", req);
}

char *kb_client_note_create_json(const char *title, const char *content, const char *tags,
                                 const char *author)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "title", title ? title : "");
   cJSON_AddStringToObject(req, "content", content ? content : "");
   if (tags && tags[0])
      cJSON_AddStringToObject(req, "tags", tags);
   if (author && author[0])
      cJSON_AddStringToObject(req, "author", author);
   return kb_v1_action_request("notes.create", req);
}

char *kb_client_note_list_json(const char *tag, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (tag && tag[0])
      cJSON_AddStringToObject(req, "tag", tag);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("notes.list", req);
}

char *kb_client_note_search_json(const char *query, int limit)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "query", query ? query : "");
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("notes.search", req);
}

/* Rules + agent telemetry + maintenance + decision_log + anti_pattern
 * + feedback wrappers live in kb_client_agent.c. */
/* The memory.* RPC wrappers (find_facts, list, get, insert, briefing,
 * context_block, ask, entity_profile, entity_edges, search_graph,
 * get_episode) live in kb_client_memory.c so this file stays under
 * the per-file line cap. */
/* moved to kb_client_memory.c */

#define KB_CLIENT_LEARNING_TIMEOUT_MS (60 * 1000)

static char *kb_v1_learning_action_request(const char *method, cJSON *req)
{
   return kb_v1_action_request_timeout(method, req, KB_CLIENT_LEARNING_TIMEOUT_MS, kb_error_json);
}

char *kb_client_learning_list_proposals_json(const char *state, const char *sink, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (state && state[0])
      cJSON_AddStringToObject(req, "state", state);
   if (sink && sink[0])
      cJSON_AddStringToObject(req, "sink", sink);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_learning_action_request("learning.list_proposals", req);
}

static char *kb_v1_learning_mutate(const char *method, int id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", id);
   return kb_v1_learning_action_request(method, req);
}

char *kb_client_learning_get_proposal_json(int id)
{
   return kb_v1_learning_mutate("learning.get_proposal", id);
}

char *kb_client_learning_accept_proposal_json(int id)
{
   return kb_v1_learning_mutate("learning.accept_proposal", id);
}

char *kb_client_learning_reject_proposal_json(int id)
{
   return kb_v1_learning_mutate("learning.reject_proposal", id);
}

/* artifacts.list_proposed / artifacts.set_state wrappers              */

char *kb_client_artifacts_list_proposed_json(const char *target_surface, int limit)
{
   cJSON *req = cJSON_CreateObject();
   if (target_surface && target_surface[0])
      cJSON_AddStringToObject(req, "target_surface", target_surface);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_learning_action_request("artifacts.list_proposed", req);
}

char *kb_client_artifact_set_state_json(const char *id, const char *new_state,
                                        const char *verdict_tag, const char *verdict_scope,
                                        const char *counter_example, const char *reason)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "id", id ? id : "");
   cJSON_AddStringToObject(req, "new_state", new_state ? new_state : "");
   if (verdict_tag && verdict_tag[0])
      cJSON_AddStringToObject(req, "verdict_tag", verdict_tag);
   if (verdict_scope && verdict_scope[0])
      cJSON_AddStringToObject(req, "verdict_scope", verdict_scope);
   if (counter_example && counter_example[0])
      cJSON_AddStringToObject(req, "counter_example", counter_example);
   if (reason && reason[0])
      cJSON_AddStringToObject(req, "reason", reason);
   return kb_v1_learning_action_request("artifacts.set_state", req);
}

/* Repair sweeps the memories table and re-upserts into pgvector; size the timeout
 * like the rebuild path (10 minutes). */
#define KB_CLIENT_MEMORY_REPAIR_TIMEOUT_MS (10 * 60 * 1000)

char *kb_client_memory_repair_json(int limit, int failed_only, int reset_stuck, int64_t memory_id,
                                   const char *embedding_command)
{
   cJSON *req = cJSON_CreateObject();
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   cJSON_AddBoolToObject(req, "failed_only", failed_only ? 1 : 0);
   cJSON_AddBoolToObject(req, "reset_stuck", reset_stuck ? 1 : 0);
   if (memory_id > 0)
      cJSON_AddNumberToObject(req, "memory_id", (double)memory_id);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   return kb_v1_action_request_timeout("memory.repair", req, KB_CLIENT_MEMORY_REPAIR_TIMEOUT_MS,
                                       kb_error_json);
}

/* Verify runs a pgvector snapshot + DB2 scan; 30s is plenty for the normal
 * path and still under any reasonable timings budget. */
#define KB_CLIENT_MEMORY_VERIFY_TIMEOUT_MS (30 * 1000)

char *kb_client_memory_verify_json(int detail, int timings, const char *embedding_command)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddBoolToObject(req, "detail", detail ? 1 : 0);
   cJSON_AddBoolToObject(req, "timings", timings ? 1 : 0);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   return kb_v1_action_request_timeout("memory.verify", req, KB_CLIENT_MEMORY_VERIFY_TIMEOUT_MS,
                                       kb_error_json);
}

/* Embed paths are batch-heavy (reembed_start walks every stale memory).
 * Share the 10-minute budget used by repair/rebuild. */
#define KB_CLIENT_MEMORY_EMBED_TIMEOUT_MS (10 * 60 * 1000)

static char *kb_v1_memory_embed_action_request(const char *method, cJSON *req)
{
   return kb_v1_action_request_timeout(method, req, KB_CLIENT_MEMORY_EMBED_TIMEOUT_MS,
                                       kb_error_json);
}

char *kb_client_memory_embed_json(int all, int64_t memory_id, const char *version,
                                  const char *embedding_command)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddBoolToObject(req, "all", all ? 1 : 0);
   if (memory_id > 0)
      cJSON_AddNumberToObject(req, "memory_id", (double)memory_id);
   if (version && version[0])
      cJSON_AddStringToObject(req, "version", version);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   return kb_v1_memory_embed_action_request("memory.embed", req);
}

char *kb_client_memory_reembed_start_json(const char *version, const char *embedding_command)
{
   cJSON *req = cJSON_CreateObject();
   if (version && version[0])
      cJSON_AddStringToObject(req, "version", version);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(req, "embedding_command", embedding_command);
   return kb_v1_memory_embed_action_request("memory.reembed_start", req);
}

char *kb_client_memory_reembed_status_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_memory_embed_action_request("memory.reembed_status", req);
}

char *kb_client_memory_reembed_cutover_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_memory_embed_action_request("memory.reembed_cutover", req);
}

char *kb_client_memory_reembed_rollback_json(const char *version)
{
   cJSON *req = cJSON_CreateObject();
   if (version && version[0])
      cJSON_AddStringToObject(req, "version", version);
   return kb_v1_memory_embed_action_request("memory.reembed_rollback", req);
}

char *kb_client_memory_scene_list_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_memory_embed_action_request("memory.scene_list", req);
}

char *kb_client_memory_scene_show_json(int64_t scene_id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "scene_id", (double)scene_id);
   return kb_v1_memory_embed_action_request("memory.scene_show", req);
}

/* Search may embed a query and hit pgvector; be generous but bounded so
 * a hung sidecar doesn't wedge the CLI indefinitely. */
#define KB_CLIENT_SEARCH_TIMEOUT_MS (2 * 60 * 1000)

const char *kb_client_v1_base_url(void)
{
   const char *url = getenv("AIMEE_KB_API_URL");
   return (url && url[0]) ? url : NULL;
}

static char *kb_client_v1_url(const char *path)
{
   const char *base = kb_client_v1_base_url();
   if (!base || !path || !path[0])
      return NULL;

   size_t base_len = strlen(base);
   while (base_len > 0 && base[base_len - 1] == '/')
      base_len--;
   size_t path_len = strlen(path);
   int path_has_slash = path[0] == '/';
   char *url = malloc(base_len + (path_has_slash ? 0 : 1) + path_len + 1);
   if (!url)
      return NULL;
   memcpy(url, base, base_len);
   size_t pos = base_len;
   if (!path_has_slash)
      url[pos++] = '/';
   memcpy(url + pos, path, path_len);
   url[pos + path_len] = '\0';
   return url;
}

/* Refuse to put the kb bearer on the wire in cleartext.
 *
 * The plain-HTTP branches below are reached only when mTLS is not configured,
 * which is legitimate for a loopback kb. It is not legitimate for a kb across a
 * network: the bearer would travel unprotected, and the thin client already
 * refuses exactly this shape. Apply the same rule here so the kb link cannot be
 * the weak one.
 *
 * Returns 1 when the request must NOT be sent. A URL with no credential to leak,
 * or an https:// URL, is allowed through unchanged. */
static int kb_plain_would_leak(const char *url)
{
   if (!url)
      return 0;
   char token[512];
   int have_token = runtime_secret_get("AIMEE_KB_API_BEARER_TOKEN", token, sizeof(token));
   runtime_secret_wipe(token, sizeof(token));
   if (!have_token)
      return 0; /* nothing to leak */

   aimee_core_endpoint_t endpoint;
   if (aimee_core_endpoint_parse(url, &endpoint) != 0)
   {
      fprintf(stderr, "kb_client: refusing to send the kb bearer to invalid endpoint '%s'\n", url);
      return 1;
   }
   if (!aimee_core_would_leak_credential(endpoint.secure, endpoint.host, "x"))
      return 0;
   /* stderr rather than aimee_log: this file links into many focused unit-test
    * binaries that do not carry the logging object. */
   fprintf(stderr,
           "kb_client: refusing to send the kb bearer in cleartext to non-loopback host '%s'; "
           "use the mTLS endpoint or terminate TLS in front of the kb\n",
           endpoint.host);
   return 1;
}

static const char *kb_client_v1_auth_header(char *buf, size_t buf_len)
{
   /* The HTTP API can run without auth; include a bearer header only when the
    * operator provides the matching client-side token.
    *
    * AIMEE_KB_CLIENT_BEARER_TOKEN is what aimee-server PRESENTS, and is read
    * first so it can be a scoped `service` credential. It falls back to
    * AIMEE_KB_API_BEARER_TOKEN — aimee-kb's own inbound token — because that is
    * what every existing deployment set, and reaching the kb matters more than
    * the ideal credential shape. But see below: falling back means presenting
    * the OWNER token, and that is worth saying out loud rather than inheriting
    * silently. */
   char token[512];
   if (!buf || buf_len == 0)
      return NULL;
   int own = runtime_secret_get("AIMEE_KB_CLIENT_BEARER_TOKEN", token, sizeof(token));
   if (!own && !runtime_secret_get("AIMEE_KB_API_BEARER_TOKEN", token, sizeof(token)))
      return NULL;

   /* An unscoped token IS the install owner on aimee-kb (kb_scope.h): it passes
    * every administrative gate. aimee-server does not need that — it needs the
    * data plane — so warn ONCE, naming the remedy, instead of running as owner
    * without anyone noticing. Not fatal: refusing here would take an upgrading
    * deployment offline over a configuration preference. */
   if (strncmp(token, "scope:", 6) != 0)
   {
      static int warned = 0;
      if (!warned)
      {
         warned = 1;
         aimee_log(LOG_WARN, "kb_client",
                   "presenting an UNSCOPED kb bearer: aimee-server is acting as the aimee-kb "
                   "install owner and passes every administrative gate. Set "
                   "AIMEE_KB_CLIENT_BEARER_TOKEN to a scoped service credential "
                   "(scope:service:<name>:<secret>, minted by the owner) to hold only the "
                   "data plane%s",
                   own ? "." : "; currently inheriting AIMEE_KB_API_BEARER_TOKEN.");
      }
   }

   char value[512];
   if (aimee_core_bearer_value(value, sizeof(value), token) != 0)
   {
      runtime_secret_wipe(token, sizeof(token));
      return NULL;
   }
   snprintf(buf, buf_len, "Authorization: %s", value);
   runtime_secret_wipe(token, sizeof(token));
   return buf;
}

/* keep_error: return the response body even on a non-2xx status.
 *
 * The default (0) frees it and returns NULL, which loses whatever the kb said
 * about WHY it refused. That is how `aimee kb reembed` reported a generic
 * "knowledge service reembed failed" while the kb was answering
 * "kb.reembed_on_dim_change is disabled; set it true...", and how
 * `aimee memory embed --all` exited 0 printing nothing against
 * "memory.embed all=true requires version". */
static char *kb_client_v1_post_json_impl(const char *path, cJSON *body, int timeout_ms,
                                         int *status_out, int keep_error)
{
   if (!kb_transport_begin(path, status_out))
      return NULL;
   char *body_json = body ? cJSON_PrintUnformatted(body) : strdup("{}");
   if (!body_json)
   {
      dependency_breaker_cancel_probe(&g_kb_dependency[kb_dependency_class_for_path(path)]);
      return NULL;
   }

   /* Distributed mode: route to the remote kb over mTLS (see get_json). */
   if (kb_client_mtls_configured())
   {
      int local_status = -1;
      int *wire_status = status_out ? status_out : &local_status;
      int64_t started_ms = kb_dependency_now_ms();
      char *r = kb_client_mtls_request_timeout("POST", path, body_json, timeout_ms, wire_status);
      kb_transport_complete_timed(path, r, *wire_status, kb_dependency_now_ms() - started_ms,
                                  timeout_ms);
      free(body_json);
      if (*wire_status < 200 || *wire_status >= 300 || !r)
      {
         if (keep_error && r)
            return r;
         free(r);
         return NULL;
      }
      return r;
   }

   char *url = kb_client_v1_url(path);
   if (url && kb_plain_would_leak(url))
   {
      free(url);
      free(body_json);
      kb_transport_complete(path, NULL, -1);
      return NULL;
   }
   if (url)
   {
      char auth[320];
      char *response = NULL;
      int status = agent_http_post(url, kb_client_v1_auth_header(auth, sizeof(auth)), body_json,
                                   &response, timeout_ms, NULL);
      if (status_out)
         *status_out = status;
      kb_transport_complete(path, response, status);
      free(body_json);
      free(url);
      if (status < 200 || status >= 300 || !response)
      {
         if (keep_error && response)
            return response;
         free(response);
         return NULL;
      }
      return response;
   }

   free(body_json);
   kb_transport_complete(path, NULL, -1);
   return NULL;
}

char *kb_client_v1_post_json(const char *path, cJSON *body, int timeout_ms, int *status_out)
{
   return kb_client_v1_post_json_impl(path, body, timeout_ms, status_out, 0);
}

char *kb_client_v1_post_json_keep_error(const char *path, cJSON *body, int timeout_ms,
                                        int *status_out)
{
   return kb_client_v1_post_json_impl(path, body, timeout_ms, status_out, 1);
}

char *kb_client_v1_post_body(const char *path, const char *body, int timeout_ms, int *status_out)
{
   return kb_client_v1_post_body_with_type(path, body, "Content-Type: application/json", timeout_ms,
                                           status_out);
}

char *kb_client_v1_post_body_with_type(const char *path, const char *body, const char *content_type,
                                       int timeout_ms, int *status_out)
{
   if (!kb_transport_begin(path, status_out))
      return NULL;

   if (kb_client_mtls_configured())
   {
      int local_status = -1;
      int *wire_status = status_out ? status_out : &local_status;
      char *r = kb_client_mtls_request_timeout_with_type("POST", path, body, content_type,
                                                         timeout_ms, wire_status);
      kb_transport_complete(path, r, *wire_status);
      if (*wire_status < 200 || *wire_status >= 300 || !r)
      {
         free(r);
         return NULL;
      }
      return r;
   }

   char *url = kb_client_v1_url(path);
   if (url && kb_plain_would_leak(url))
   {
      free(url);
      kb_transport_complete(path, NULL, -1);
      return NULL;
   }
   if (url)
   {
      char auth[320];
      char *response = NULL;
      int status =
          agent_http_post_content_type(url, kb_client_v1_auth_header(auth, sizeof(auth)),
                                       content_type, body ? body : "", &response, timeout_ms, NULL);
      if (status_out)
         *status_out = status;
      kb_transport_complete(path, response, status);
      free(url);
      if (status < 200 || status >= 300 || !response)
      {
         free(response);
         return NULL;
      }
      return response;
   }

   kb_transport_complete(path, NULL, -1);
   return NULL;
}

char *kb_client_v1_get_json(const char *path, int timeout_ms, int *status_out)
{
   if (!kb_transport_begin(path, status_out))
      return NULL;

   /* Distributed mode: a configured remote kb (AIMEE_KB_CONN) is reached over
    * mTLS with the enrollment-issued client cert, ahead of HTTP-URL / socket. */
   if (kb_client_mtls_configured())
   {
      int local_status = -1;
      int *wire_status = status_out ? status_out : &local_status;
      char *r = kb_client_mtls_request_timeout("GET", path, NULL, timeout_ms, wire_status);
      kb_transport_complete(path, r, *wire_status);
      if (*wire_status < 200 || *wire_status >= 300 || !r)
      {
         free(r);
         return NULL;
      }
      return r;
   }

   char *url = kb_client_v1_url(path);
   if (url && kb_plain_would_leak(url))
   {
      free(url);
      kb_transport_complete(path, NULL, -1);
      return NULL;
   }
   if (url)
   {
      char auth[320];
      char *response = NULL;
      int status =
          agent_http_get(url, kb_client_v1_auth_header(auth, sizeof(auth)), &response, timeout_ms);
      if (status_out)
         *status_out = status;
      kb_transport_complete(path, response, status);
      free(url);
      if (status < 200 || status >= 300 || !response)
      {
         free(response);
         return NULL;
      }
      return response;
   }

   kb_transport_complete(path, NULL, -1);
   return NULL;
}

char *kb_client_search_json(const char *project, const char *query, const char *embedding_command,
                            int max_results, const char *format)
{
   return kb_client_search_json_ex(project, query, embedding_command, max_results, format, NULL);
}

char *kb_client_search_json_ex(const char *project, const char *query,
                               const char *embedding_command, int max_results, const char *format,
                               const char *fusion_mode_override)
{
   return kb_client_search_json_scoped_ex(project, !project || !project[0], query,
                                          embedding_command, max_results, format,
                                          fusion_mode_override);
}

char *kb_client_search_json_scoped_ex(const char *project, int all_projects, const char *query,
                                      const char *embedding_command, int max_results,
                                      const char *format, const char *fusion_mode_override)
{
   if (!query || !query[0])
      return kb_error_json("kb.search requires query");

   /* Short-TTL result cache (kb_client_cache.c): a hit skips the kb round-trip;
    * the kb_client_ws subscriber flushes the cache on every /v1/events
    * invalidation so results never outlive a release/ingest change. No-op
    * unless AIMEE_KB_CACHE_TTL_S (or config) enables it. */
   char cache_key[480];
   int have_key =
       snprintf(cache_key, sizeof(cache_key), "search|%s|%d|%s|%d|%s|%s", query, max_results,
                project ? project : "", all_projects, format ? format : "",
                fusion_mode_override ? fusion_mode_override : "") < (int)sizeof(cache_key);
   if (have_key)
   {
      char *cached = kb_cache_get(cache_key);
      if (cached)
      {
         int valid = 0;
         g_kb_last_result = kb_classify_json_result("/v1/search", cached, &valid);
         if (!valid)
            g_kb_last_result = KB_CLIENT_RESULT_UNAVAILABLE;
         return cached;
      }
   }

   cJSON *body = cJSON_CreateObject();
   if (!body)
      return kb_error_json("out of memory");
   if (project && project[0])
      cJSON_AddStringToObject(body, "project", project);
   if (all_projects)
      /* NULL is an intentional all-project call at this internal boundary.
       * Public request surfaces must resolve an active project first; spelling
       * this out prevents the KB from treating an omitted scope as global. */
      cJSON_AddStringToObject(body, "scope", "all");
   cJSON_AddStringToObject(body, "query", query);
   if (embedding_command && embedding_command[0])
      cJSON_AddStringToObject(body, "embedding_command", embedding_command);
   cJSON_AddNumberToObject(body, "max_results", max_results);
   if (format && format[0])
      cJSON_AddStringToObject(body, "format", format);
   if (fusion_mode_override && fusion_mode_override[0])
      cJSON_AddStringToObject(body, "fusion_mode", fusion_mode_override);
   int http_status = -1;
   char *resp =
       kb_client_v1_post_json("/v1/search", body, KB_CLIENT_SEARCH_TIMEOUT_MS, &http_status);
   cJSON_Delete(body);
   if (resp)
   {
      if (have_key)
         kb_cache_put(cache_key, resp);
      return resp;
   }
   if (http_status >= 100)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "knowledge service /v1/search returned HTTP %d", http_status);
      return kb_typed_error_json(kb_client_last_result_status(), msg);
   }
   return kb_typed_error_json(kb_client_last_result_status(),
                              "knowledge service /v1/search did not respond");
}

char *kb_client_curator_implements_json(const char *topic)
{
   cJSON *b = cJSON_CreateObject();
   if (!b)
      return NULL;
   cJSON_AddStringToObject(b, "topic", topic ? topic : "");
   int http = -1;
   char *r = kb_client_v1_post_json("/v1/implements", b, CLIENT_DEFAULT_TIMEOUT_MS, &http);
   cJSON_Delete(b);
   return r;
}

char *kb_client_curator_synthesize_json(const char *topic)
{
   cJSON *b = cJSON_CreateObject();
   if (!b)
      return NULL;
   cJSON_AddStringToObject(b, "topic", topic ? topic : "");
   int http = -1;
   char *r = kb_client_v1_post_json("/v1/synthesize", b, CLIENT_DEFAULT_TIMEOUT_MS, &http);
   cJSON_Delete(b);
   return r;
}

char *kb_client_curator_contradictions_json(int limit)
{
   cJSON *b = cJSON_CreateObject();
   if (!b)
      return NULL;
   cJSON_AddNumberToObject(b, "limit", limit);
   int http = -1;
   char *r = kb_client_v1_post_json("/v1/contradictions", b, CLIENT_DEFAULT_TIMEOUT_MS, &http);
   cJSON_Delete(b);
   return r;
}

char *kb_client_vector_status_json(void)
{
   char *status_json = kb_client_status_json();
   if (!status_json)
      return kb_vector_unavailable_json("knowledge service unavailable");

   cJSON *root = cJSON_Parse(status_json);
   free(status_json);
   if (!root)
      return kb_vector_unavailable_json("invalid knowledge service response");

   cJSON *vector = cJSON_GetObjectItemCaseSensitive(root, "vector");
   char *json = NULL;
   if (vector)
      json = cJSON_PrintUnformatted(vector);
   cJSON_Delete(root);
   return json ? json : kb_vector_unavailable_json("knowledge service returned no vector status");
}

char *kb_client_workers_json(void)
{
   return kb_client_v1_get_json("/v1/workers", CLIENT_DEFAULT_TIMEOUT_MS, NULL);
}

int kb_client_canonical_index_scan(const char *project, const char *root_path, int force)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "project", project ? project : "");
   cJSON_AddStringToObject(req, "root_path", root_path ? root_path : "");
   if (force)
      cJSON_AddTrueToObject(req, "force");
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/code/scan", req, 60000, &http_status);
   cJSON_Delete(req);
   int ok = (resp != NULL);
   free(resp);
   return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Intelligence readiness queries                                       */
/* ------------------------------------------------------------------ */

char *kb_client_calibrate_readiness_json(void)
{
   char *json = kb_client_v1_get_json("/v1/intelligence/calibration/readiness",
                                      CLIENT_DEFAULT_TIMEOUT_MS, NULL);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

char *kb_client_demote_check_json(void)
{
   char *json =
       kb_client_v1_get_json("/v1/intelligence/demotion/check", CLIENT_DEFAULT_TIMEOUT_MS, NULL);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

char *kb_client_ranker_export_view_json(void)
{
   char *json = kb_client_v1_get_json("/v1/intelligence/ranker/export-view",
                                      CLIENT_DEFAULT_TIMEOUT_MS, NULL);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

char *kb_client_ranker_fit_json(void)
{
   /* Fitting materializes the view, spawns the sidecar, and runs the benchmark
    * gate — allow a long window (mirrors the repair-class maintenance timeout). */
   cJSON *req = cJSON_CreateObject();
   char *json = kb_client_v1_post_json("/v1/intelligence/ranker/fit", req,
                                       KB_CLIENT_REPAIR_TIMEOUT_MS, NULL);
   cJSON_Delete(req);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

char *kb_client_bandit_export_json(void)
{
   char *json =
       kb_client_v1_get_json("/v1/intelligence/bandit/export", CLIENT_DEFAULT_TIMEOUT_MS, NULL);
   return json ? json : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

char *kb_client_bandit_replay_record_json(const char *decision_point, const char *result_json)
{
   if (!decision_point || !decision_point[0] || !result_json || !result_json[0])
      return strdup("{\"status\":\"error\",\"message\":\"decision_point and result required\"}");

   cJSON *body = cJSON_CreateObject();
   if (!body)
      return strdup("{\"status\":\"error\",\"message\":\"out of memory\"}");
   cJSON_AddStringToObject(body, "decision_point", decision_point);

   cJSON *result = cJSON_Parse(result_json);
   if (!result || !cJSON_IsObject(result))
   {
      cJSON_Delete(result);
      cJSON_Delete(body);
      return strdup("{\"status\":\"error\",\"message\":\"result must be a JSON object\"}");
   }
   cJSON_AddItemToObject(body, "result", result);

   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/intelligence/bandit/replay-record", body,
                                       CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   cJSON_Delete(body);
   return resp ? resp : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

/* Sample an arm for a server-side decision point via the kb DB2 bandit. On
 * success (status "ok"), fills arm_out + decision_id_out and returns 0. Returns
 * -1 when sampling is disabled (no optimize command), on transport failure, or
 * on a malformed response — the caller falls back to its default behaviour. */
int kb_client_bandit_sample(const char *decision_point, const char *const *arms, int n_arms,
                            char *arm_out, size_t arm_out_len, char *decision_id_out,
                            size_t decision_id_out_len)
{
   if (arm_out && arm_out_len)
      arm_out[0] = '\0';
   if (decision_id_out && decision_id_out_len)
      decision_id_out[0] = '\0';
   if (!decision_point || !decision_point[0] || !arms || n_arms < 1)
      return -1;

   cJSON *body = cJSON_CreateObject();
   if (!body)
      return -1;
   cJSON_AddStringToObject(body, "decision_point", decision_point);
   cJSON *aj = cJSON_CreateArray();
   for (int i = 0; i < n_arms; i++)
      if (arms[i] && arms[i][0])
         cJSON_AddItemToArray(aj, cJSON_CreateString(arms[i]));
   cJSON_AddItemToObject(body, "arms", aj);

   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/intelligence/bandit/sample", body,
                                       CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   cJSON_Delete(body);
   if (!resp)
      return -1;
   cJSON *r = cJSON_Parse(resp);
   free(resp);
   int rc = -1;
   if (r)
   {
      const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(r, "status"));
      const char *arm = cJSON_GetStringValue(cJSON_GetObjectItem(r, "arm"));
      const char *did = cJSON_GetStringValue(cJSON_GetObjectItem(r, "decision_id"));
      if (status && strcmp(status, "ok") == 0 && arm && did)
      {
         if (arm_out && arm_out_len)
            snprintf(arm_out, arm_out_len, "%s", arm);
         if (decision_id_out && decision_id_out_len)
            snprintf(decision_id_out, decision_id_out_len, "%s", did);
         rc = 0;
      }
   }
   cJSON_Delete(r);
   return rc;
}

/* Persist the production-default arm for a decision point. Returns the raw kb
 * response JSON (caller frees), which carries {status, rollback_arm}. */
char *kb_client_bandit_promote_json(const char *decision_point, const char *arm)
{
   if (!decision_point || !decision_point[0] || !arm || !arm[0])
      return strdup("{\"status\":\"error\",\"message\":\"decision_point and arm required\"}");
   cJSON *body = cJSON_CreateObject();
   if (!body)
      return strdup("{\"status\":\"error\",\"message\":\"out of memory\"}");
   cJSON_AddStringToObject(body, "decision_point", decision_point);
   cJSON_AddStringToObject(body, "arm", arm);
   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/intelligence/bandit/promote", body,
                                       CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   cJSON_Delete(body);
   return resp ? resp : strdup("{\"status\":\"error\",\"message\":\"no response\"}");
}

/* Close a sampled decision with its observed reward [0,1]. Best-effort: a failed
 * close just means a missed learning sample, never a caller error. Returns 0. */
int kb_client_bandit_close(const char *decision_point, const char *decision_id, const char *arm_id,
                           double reward)
{
   if (!decision_point || !decision_point[0] || !decision_id || !decision_id[0] || !arm_id ||
       !arm_id[0])
      return -1;
   cJSON *body = cJSON_CreateObject();
   if (!body)
      return -1;
   cJSON_AddStringToObject(body, "decision_point", decision_point);
   cJSON_AddStringToObject(body, "decision_id", decision_id);
   cJSON_AddStringToObject(body, "arm_id", arm_id);
   cJSON_AddNumberToObject(body, "reward", reward);

   int http_status = -1;
   char *resp = kb_client_v1_post_json("/v1/intelligence/bandit/close", body,
                                       CLIENT_DEFAULT_TIMEOUT_MS, &http_status);
   cJSON_Delete(body);
   free(resp); /* best-effort */
   return 0;
}
