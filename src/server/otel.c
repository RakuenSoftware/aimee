/* otel.c: OpenTelemetry trace export (OTLP/HTTP JSON).
 *
 * Batches completed spans in a mutex-protected ring buffer, then exports
 * them to {endpoint}/v1/traces as JSON OTLP when:
 *   - an agent turn completes (otel_on_trace direction="response"), or
 *   - the buffer reaches OTEL_MAX_PENDING, or
 *   - otel_flush() is called explicitly.
 *
 * Export failures are silently logged at DEBUG level and dropped.
 * The agent loop is never blocked: POST timeout is capped at 2 s.
 */
#include "aimee.h"
#include "otel.h"
#include "agent_exec.h"
#include "log.h"
#include "platform_random.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ state */

static char g_endpoint[512];    /* empty = disabled */
static char g_service_name[64]; /* e.g. "aimee" */
static char g_trace_id[33];     /* session-level trace ID (128-bit hex) */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* Span ring buffer */
static otel_span_t g_pending[OTEL_MAX_PENDING];
static int g_pending_count = 0;

/* Thread-local turn span tracking */
_Thread_local static char tl_turn_span_id[17] = {0};
_Thread_local static unsigned long long tl_turn_start_ns = 0;
_Thread_local static int tl_turn_number = -1;

/* ------------------------------------------------------------------ helpers */

static unsigned long long now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

static int gen_hex_id(char *buf, size_t bytes)
{
   /* bytes random bytes → 2*bytes hex chars + NUL */
   unsigned char raw[16];
   if (bytes > sizeof(raw))
      bytes = sizeof(raw);
   if (platform_random_bytes(raw, bytes) != 0)
   {
      buf[0] = '\0';
      return -1;
   }
   for (size_t i = 0; i < bytes; i++)
      snprintf(buf + 2 * i, 3, "%02x", raw[i]);
   buf[2 * bytes] = '\0';
   return 0;
}

/* ------------------------------------------------------------------ init */

void otel_init(const char *endpoint, const char *service_name, const char *session_id)
{
   pthread_mutex_lock(&g_mu);
   if (endpoint && endpoint[0])
   {
      snprintf(g_endpoint, sizeof(g_endpoint), "%s", endpoint);
      snprintf(g_service_name, sizeof(g_service_name), "%s",
               (service_name && service_name[0]) ? service_name : "aimee");

      /* Derive a 128-bit trace ID from the session ID.  Re-use the session UUID
       * hex digits (strip dashes) so the trace ID is stable across process
       * boundaries within one session. */
      if (session_id && session_id[0])
      {
         int out = 0;
         for (int i = 0; session_id[i] && out < 32; i++)
         {
            char c = session_id[i];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
               g_trace_id[out++] = (char)(c | 0x20); /* lower-case hex */
         }
         /* Pad to 32 chars if the session ID was short */
         while (out < 32)
            g_trace_id[out++] = '0';
         g_trace_id[32] = '\0';
      }
      else
      {
         if (gen_hex_id(g_trace_id, 16) != 0)
            g_endpoint[0] = '\0';
      }
   }
   else
   {
      g_endpoint[0] = '\0';
   }
   pthread_mutex_unlock(&g_mu);
}

int otel_is_enabled(void)
{
   return g_endpoint[0] != '\0';
}

/* ------------------------------------------------------------------ span API */

void otel_span_start(otel_span_t *span, const char *name, const char *parent_span_id)
{
   if (!span)
      return;
   memset(span, 0, sizeof(*span));

   pthread_mutex_lock(&g_mu);
   snprintf(span->trace_id, sizeof(span->trace_id), "%s", g_trace_id);
   pthread_mutex_unlock(&g_mu);

   if (gen_hex_id(span->span_id, 8) != 0)
      return;

   if (parent_span_id && parent_span_id[0])
      snprintf(span->parent_span_id, sizeof(span->parent_span_id), "%s", parent_span_id);

   snprintf(span->name, sizeof(span->name), "%s", name ? name : "span");
   span->start_ns = now_ns();
   span->status_code = 0;
   span->attr_count = 0;
}

void otel_span_attr_s(otel_span_t *span, const char *key, const char *value)
{
   if (!span || !key || span->attr_count >= OTEL_MAX_ATTRS)
      return;
   snprintf(span->attrs[span->attr_count].k, sizeof(span->attrs[0].k), "%s", key);
   snprintf(span->attrs[span->attr_count].v, sizeof(span->attrs[0].v), "%s", value ? value : "");
   span->attr_count++;
}

void otel_span_attr_i(otel_span_t *span, const char *key, long value)
{
   char buf[32];
   snprintf(buf, sizeof(buf), "%ld", value);
   otel_span_attr_s(span, key, buf);
}

void otel_span_end(otel_span_t *span, int error)
{
   if (!span || !otel_is_enabled())
      return;
   span->end_ns = now_ns();
   span->status_code = error ? 2 : 1;

   pthread_mutex_lock(&g_mu);
   if (g_pending_count >= OTEL_MAX_PENDING)
   {
      /* Buffer full — drop oldest span to make room; caller should flush soon */
      memmove(g_pending, g_pending + 1, (size_t)(OTEL_MAX_PENDING - 1) * sizeof(otel_span_t));
      g_pending_count = OTEL_MAX_PENDING - 1;
   }
   g_pending[g_pending_count++] = *span;
   pthread_mutex_unlock(&g_mu);
}

/* ------------------------------------------------------------------ JSON builder */

/* Append one span to a cJSON-style string buffer.
 * Returns 1 on success, 0 on allocation failure. */
static int append_span_json(char **buf, size_t *cap, size_t *len, const otel_span_t *s)
{
   /* Each span is at most ~4KB with long attributes */
   const size_t CHUNK = 4096;

   /* Ensure enough room */
   if (*len + CHUNK >= *cap)
   {
      size_t new_cap = *cap + CHUNK * 4;
      char *nb = realloc(*buf, new_cap);
      if (!nb)
         return 0;
      *buf = nb;
      *cap = new_cap;
   }

   /* Build the span JSON object */
   char tmp[512];
   size_t n;

   /* Open object */
   (*buf)[(*len)++] = '{';

   /* traceId */
   n = (size_t)snprintf(tmp, sizeof(tmp), "\"traceId\":\"%s\"", s->trace_id);
   if (*len + n + 1 >= *cap)
   {
      size_t nc = *cap + n + 512;
      char *nb = realloc(*buf, nc);
      if (!nb)
         return 0;
      *buf = nb;
      *cap = nc;
   }
   memcpy(*buf + *len, tmp, n);
   *len += n;

   /* spanId */
   n = (size_t)snprintf(tmp, sizeof(tmp), ",\"spanId\":\"%s\"", s->span_id);
   if (*len + n + 1 >= *cap)
   {
      size_t nc = *cap + n + 512;
      char *nb = realloc(*buf, nc);
      if (!nb)
         return 0;
      *buf = nb;
      *cap = nc;
   }
   memcpy(*buf + *len, tmp, n);
   *len += n;

   /* parentSpanId (only when set) */
   if (s->parent_span_id[0])
   {
      n = (size_t)snprintf(tmp, sizeof(tmp), ",\"parentSpanId\":\"%s\"", s->parent_span_id);
      if (*len + n + 1 >= *cap)
      {
         size_t nc = *cap + n + 512;
         char *nb = realloc(*buf, nc);
         if (!nb)
            return 0;
         *buf = nb;
         *cap = nc;
      }
      memcpy(*buf + *len, tmp, n);
      *len += n;
   }

   /* name, kind (INTERNAL=1), times */
   n = (size_t)snprintf(tmp, sizeof(tmp),
                        ",\"name\":\"%s\",\"kind\":1"
                        ",\"startTimeUnixNano\":\"%llu\""
                        ",\"endTimeUnixNano\":\"%llu\"",
                        s->name, s->start_ns, s->end_ns);
   if (*len + n + 1 >= *cap)
   {
      size_t nc = *cap + n + 512;
      char *nb = realloc(*buf, nc);
      if (!nb)
         return 0;
      *buf = nb;
      *cap = nc;
   }
   memcpy(*buf + *len, tmp, n);
   *len += n;

   /* status */
   n = (size_t)snprintf(tmp, sizeof(tmp), ",\"status\":{\"code\":%d}", s->status_code);
   memcpy(*buf + *len, tmp, n);
   *len += n;

   /* attributes */
   if (s->attr_count > 0)
   {
      if (*len + 64 >= *cap)
      {
         size_t nc = *cap + 1024;
         char *nb = realloc(*buf, nc);
         if (!nb)
            return 0;
         *buf = nb;
         *cap = nc;
      }
      memcpy(*buf + *len, ",\"attributes\":[", 15);
      *len += 15;

      for (int i = 0; i < s->attr_count; i++)
      {
         char attr[640];
         /* JSON-escape key and value (minimal: just replace '"' and '\') */
         n = (size_t)snprintf(attr, sizeof(attr),
                              "%s{\"key\":\"%s\",\"value\":{\"stringValue\":\"%s\"}}", i ? "," : "",
                              s->attrs[i].k, s->attrs[i].v);
         if (*len + n + 4 >= *cap)
         {
            size_t nc = *cap + n + 512;
            char *nb = realloc(*buf, nc);
            if (!nb)
               return 0;
            *buf = nb;
            *cap = nc;
         }
         memcpy(*buf + *len, attr, n);
         *len += n;
      }

      if (*len + 4 >= *cap)
      {
         size_t nc = *cap + 64;
         char *nb = realloc(*buf, nc);
         if (!nb)
            return 0;
         *buf = nb;
         *cap = nc;
      }
      (*buf)[(*len)++] = ']';
   }

   /* Close object */
   if (*len + 4 >= *cap)
   {
      size_t nc = *cap + 64;
      char *nb = realloc(*buf, nc);
      if (!nb)
         return 0;
      *buf = nb;
      *cap = nc;
   }
   (*buf)[(*len)++] = '}';
   (*buf)[*len] = '\0';
   return 1;
}

/* Build the full OTLP JSON payload from the pending buffer.
 * Caller must hold g_mu. */
static char *build_otlp_json(const otel_span_t *spans, int count, const char *svc_name)
{
   /* Initial capacity: ~4KB per span */
   size_t cap = (size_t)count * 4096 + 512;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t len = 0;

   /* OTLP/HTTP JSON envelope */
   const char *hdr1 = "{\"resourceSpans\":[{\"resource\":{\"attributes\":["
                      "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"";
   const char *hdr2 = "\"}}]},\"scopeSpans\":[{\"scope\":{\"name\":\"aimee.agent\"},\"spans\":[";
   const char *ftr = "]}]}]}";

   size_t h1 = strlen(hdr1);
   size_t sn = strlen(svc_name);
   size_t h2 = strlen(hdr2);
   size_t need = h1 + sn + h2 + strlen(ftr) + 4;
   if (need >= cap)
   {
      cap = need + (size_t)count * 4096;
      char *nb = realloc(buf, cap);
      if (!nb)
      {
         free(buf);
         return NULL;
      }
      buf = nb;
   }
   memcpy(buf + len, hdr1, h1);
   len += h1;
   memcpy(buf + len, svc_name, sn);
   len += sn;
   memcpy(buf + len, hdr2, h2);
   len += h2;

   for (int i = 0; i < count; i++)
   {
      if (i > 0)
      {
         if (len + 4 >= cap)
         {
            size_t nc = cap + 64;
            char *nb = realloc(buf, nc);
            if (!nb)
            {
               free(buf);
               return NULL;
            }
            buf = nb;
            cap = nc;
         }
         buf[len++] = ',';
         buf[len] = '\0';
      }
      if (!append_span_json(&buf, &cap, &len, &spans[i]))
      {
         free(buf);
         return NULL;
      }
   }

   size_t f = strlen(ftr);
   if (len + f + 4 >= cap)
   {
      size_t nc = cap + f + 16;
      char *nb = realloc(buf, nc);
      if (!nb)
      {
         free(buf);
         return NULL;
      }
      buf = nb;
      cap = nc;
   }
   memcpy(buf + len, ftr, f);
   len += f;
   buf[len] = '\0';
   (void)cap;
   return buf;
}

/* ------------------------------------------------------------------ flush */

void otel_flush(void)
{
   if (!otel_is_enabled())
      return;

   /* Drain the buffer under the lock */
   pthread_mutex_lock(&g_mu);
   if (g_pending_count == 0)
   {
      pthread_mutex_unlock(&g_mu);
      return;
   }

   /* Copy spans out so we can release the lock before the HTTP call */
   int count = g_pending_count;
   otel_span_t *copy = malloc((size_t)count * sizeof(otel_span_t));
   if (!copy)
   {
      g_pending_count = 0;
      pthread_mutex_unlock(&g_mu);
      return;
   }
   memcpy(copy, g_pending, (size_t)count * sizeof(otel_span_t));
   g_pending_count = 0;
   char endpoint[sizeof(g_endpoint)];
   snprintf(endpoint, sizeof(endpoint), "%s", g_endpoint);
   char svc_name[sizeof(g_service_name)];
   snprintf(svc_name, sizeof(svc_name), "%s", g_service_name);
   pthread_mutex_unlock(&g_mu);

   char *payload = build_otlp_json(copy, count, svc_name);
   free(copy);
   if (!payload)
      return;

   /* POST to {endpoint}/v1/traces */
   char url[sizeof(g_endpoint) + 16];
   snprintf(url, sizeof(url), "%s/v1/traces", endpoint);

   char *response = NULL;
   int http_status =
       agent_http_post(url, NULL,                /* no auth header */
                       payload, &response, 2000, /* 2 s timeout — never block the loop */
                       "Content-Type: application/json");
   free(payload);
   free(response);

   if (http_status < 0 || (http_status != 200 && http_status != 204))
   {
      LOG_DEBUG("otel", "export to %s returned %d (dropped %d spans)", url, http_status, count);
   }
   else
   {
      LOG_DEBUG("otel", "exported %d spans to %s", count, url);
   }
}

/* ------------------------------------------------------------------ trace hook */

void otel_on_trace(const char *direction, const char *tool_name, const char *tool_args,
                   const char *tool_result, int turn)
{
   if (!otel_is_enabled() || !direction)
      return;

   if (strcmp(direction, "request") == 0)
   {
      /* Beginning of a new LLM turn — record the start so we can emit a
       * turn span when the response arrives. */
      tl_turn_start_ns = now_ns();
      tl_turn_number = turn;
      /* Generate a new span_id for this turn */
      gen_hex_id(tl_turn_span_id, 8);
   }
   else if (strcmp(direction, "response") == 0)
   {
      /* LLM responded — close and enqueue the turn span. */
      if (tl_turn_start_ns == 0)
         return; /* no matching request seen */

      otel_span_t span;
      memset(&span, 0, sizeof(span));

      pthread_mutex_lock(&g_mu);
      snprintf(span.trace_id, sizeof(span.trace_id), "%s", g_trace_id);
      pthread_mutex_unlock(&g_mu);

      snprintf(span.span_id, sizeof(span.span_id), "%s", tl_turn_span_id);
      snprintf(span.name, sizeof(span.name), "agent.turn");
      span.start_ns = tl_turn_start_ns;
      span.end_ns = now_ns();
      span.status_code = 1;

      otel_span_attr_i(&span, "turn", tl_turn_number);

      pthread_mutex_lock(&g_mu);
      if (g_pending_count >= OTEL_MAX_PENDING)
      {
         memmove(g_pending, g_pending + 1, (size_t)(OTEL_MAX_PENDING - 1) * sizeof(otel_span_t));
         g_pending_count = OTEL_MAX_PENDING - 1;
      }
      g_pending[g_pending_count++] = span;
      pthread_mutex_unlock(&g_mu);

      /* Reset thread-local state */
      tl_turn_start_ns = 0;
      tl_turn_number = -1;

      /* Flush at turn boundary — compact batch for this LLM round */
      otel_flush();
   }
   else if (strcmp(direction, "tool_call") == 0)
   {
      /* A tool was dispatched and completed — emit one span for it. */
      if (!tool_name)
         return;

      otel_span_t span;
      otel_span_start(&span, "tool_call", tl_turn_span_id[0] ? tl_turn_span_id : NULL);
      otel_span_attr_s(&span, "tool.name", tool_name);

      /* Truncate args to 240 chars to stay within attr value limit */
      if (tool_args && tool_args[0])
      {
         char snippet[241];
         snprintf(snippet, sizeof(snippet), "%s", tool_args);
         otel_span_attr_s(&span, "tool.args", snippet);
      }

      /* Detect error in tool result */
      int error = 0;
      if (tool_result && strncmp(tool_result, "error", 5) == 0)
         error = 1;

      if (tool_result && tool_result[0])
      {
         char snippet[241];
         snprintf(snippet, sizeof(snippet), "%s", tool_result);
         otel_span_attr_s(&span, "tool.result_snippet", snippet);
      }

      otel_span_attr_i(&span, "turn", turn);
      otel_span_end(&span, error);
   }
}

/* ------------------------------------------------------------------ delegation */

void otel_delegation_start(otel_span_t *span, const char *role, const char *prompt_snippet)
{
   if (!span || !otel_is_enabled())
      return;
   otel_span_start(span, "delegate", tl_turn_span_id[0] ? tl_turn_span_id : NULL);
   otel_span_attr_s(span, "delegate.role", role ? role : "");
   if (prompt_snippet && prompt_snippet[0])
   {
      char snippet[241];
      snprintf(snippet, sizeof(snippet), "%s", prompt_snippet);
      otel_span_attr_s(span, "delegate.prompt", snippet);
   }
}

void otel_delegation_end(otel_span_t *span, int error)
{
   if (!span || !otel_is_enabled())
      return;
   otel_span_end(span, error);
   otel_flush();
}
