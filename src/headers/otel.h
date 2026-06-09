/* otel.h: OpenTelemetry trace export (OTLP/HTTP JSON).
 *
 * Collects spans for agent sessions, turns, tool calls, and delegations,
 * then exports them to a configurable OTLP endpoint.  Zero overhead when
 * the endpoint is not configured (all functions are no-ops).
 *
 * Span model:
 *   Session Span (root)
 *     └─ Agent Turn Span
 *          ├─ Tool Call Span
 *          └─ Delegation Span
 *
 * Export: JSON OTLP (not protobuf) to {endpoint}/v1/traces.
 * Failures are silently dropped — the agent loop is never blocked.
 */

#ifndef AIMEE_OTEL_H
#define AIMEE_OTEL_H 1

/* Maximum attributes per span */
#define OTEL_MAX_ATTRS 12

/* Maximum spans buffered before an auto-flush */
#define OTEL_MAX_PENDING 128

typedef struct
{
   char trace_id[33];       /* 128-bit lowercase hex, null-terminated */
   char span_id[17];        /* 64-bit  lowercase hex, null-terminated */
   char parent_span_id[17]; /* empty string when this is a root span  */
   char name[128];
   unsigned long long start_ns; /* nanoseconds since Unix epoch */
   unsigned long long end_ns;
   int status_code; /* 0=unset, 1=ok, 2=error */

   struct
   {
      char k[64];
      char v[256];
   } attrs[OTEL_MAX_ATTRS];
   int attr_count;
} otel_span_t;

/* Initialise with the OTLP endpoint base URL (e.g. "http://host:4318").
 * Pass an empty string or NULL to disable export entirely.
 * Safe to call multiple times; last call wins. */
void otel_init(const char *endpoint, const char *service_name, const char *session_id);

/* Returns 1 if export is enabled (endpoint was set), 0 otherwise. */
int otel_is_enabled(void);

/* Begin a span.  Fills span->trace_id (from the session trace), span->span_id
 * (random), and span->start_ns.  parent_span_id may be NULL or empty for a
 * root span. */
void otel_span_start(otel_span_t *span, const char *name, const char *parent_span_id);

/* Add a string attribute.  Silently ignored when attr_count == OTEL_MAX_ATTRS. */
void otel_span_attr_s(otel_span_t *span, const char *key, const char *value);

/* Add an integer attribute (stored as string). */
void otel_span_attr_i(otel_span_t *span, const char *key, long value);

/* Record end time, set status, and enqueue the span for export.
 * error != 0 → status ERROR; error == 0 → status OK.
 * If the buffer is full, an immediate flush is attempted before enqueuing. */
void otel_span_end(otel_span_t *span, int error);

/* Export all buffered spans to the OTLP endpoint and clear the buffer.
 * Safe to call from any thread.  No-op when disabled. */
void otel_flush(void);

/* Convenience: called by agent_trace_log to emit spans without cluttering
 * the trace-log function body.
 *   direction  "request"   → start a new turn span (stored thread-locally)
 *   direction  "response"  → end the current turn span and flush
 *   direction  "tool_call" → emit a child span for a single tool execution
 * tool_name / tool_args / tool_result are used for tool_call spans only. */
void otel_on_trace(const char *direction, const char *tool_name, const char *tool_args,
                   const char *tool_result, int turn);

/* Convenience wrappers for delegation spans.
 * Call otel_delegation_start() before running the delegate; capture the
 * returned span and pass it to otel_delegation_end() when done. */
void otel_delegation_start(otel_span_t *span, const char *role, const char *prompt_snippet);
void otel_delegation_end(otel_span_t *span, int error);

#endif /* AIMEE_OTEL_H */
