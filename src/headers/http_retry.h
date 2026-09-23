#ifndef DEC_HTTP_RETRY_H
#define DEC_HTTP_RETRY_H 1

#include <stddef.h>

/* http_retry: exponential backoff with overflow-safe retries for LLM API calls.
 *
 * Retryable status codes map through failover_classify()/failover_action().
 * The current backoff-compatible set remains 408, 409, 429, 500, 502, 503, 504.
 * Non-retryable (fail immediately): 400, 401, 403, 404, and all other 4xx.
 * Network errors are retryable; local admission refusal is not.
 */

/* Default retry parameters */
#define HTTP_RETRY_MAX_ATTEMPTS 3
#define HTTP_RETRY_BASE_MS      1000
#define HTTP_RETRY_MAX_MS       30000

/* Some OpenAI-compatible local/provider gateways return HTTP 503 with a
 * "Loading model" body while the target model warms. Keep the general default
 * conservative, but allow that explicit warmup state to wait longer. */
#define HTTP_RETRY_MODEL_LOADING_MAX_ATTEMPTS 8

/* Returns 1 if the HTTP status code is retryable, 0 otherwise.
 * Negative network status is retryable except HTTP_RETRY_ADMISSION_REFUSED. */
int http_should_retry(int http_status);

/* Returns 1 when a response body is the provider's transient model-warmup
 * signal rather than a generic server failure. */
int http_response_is_model_loading(const char *response_body);

/* Compute backoff delay in milliseconds for the given attempt (0-based).
 * Uses exponential backoff (base_ms * 2^attempt) clamped to max_ms.
 * Overflow-safe: will not exceed max_ms regardless of attempt count. */
int http_backoff_ms(int attempt, int base_ms, int max_ms);

/* Retry wrapper around agent_http_post().
 * Retries up to max_attempts times on retryable status codes.
 * Sleeps with exponential backoff between retries.
 * Logs retry attempts at INFO level to stderr.
 * Returns the final HTTP status code, or -1 on network error.
 * On success or non-retryable error, *response_buf holds the response body. */
int http_retry_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers, int max_attempts, int base_ms,
                    int max_ms);

/* Context-aware retry wrapper. Provider/model/session metadata is optional;
 * when present, non-success classifications are recorded as DB1
 * failover_event rows. */
int http_retry_post_context(const char *url, const char *auth_header, const char *body,
                            char **response_buf, int timeout_ms, const char *extra_headers,
                            int max_attempts, int base_ms, int max_ms, const char *provider,
                            const char *model, const char *session_id);
int http_retry_post_context_bytes(const char *url, const char *auth_header, const void *body,
                                  size_t body_len, char **response_buf, int timeout_ms,
                                  const char *extra_headers, int max_attempts, int base_ms,
                                  int max_ms, const char *provider, const char *model,
                                  const char *session_id);

/* An admission refusal is local and must never trigger provider failover. */
#define HTTP_RETRY_ADMISSION_REFUSED (-2)
typedef int (*http_retry_admit_cb_t)(void);

/* The caller has admitted the first attempt. Before each resend, after backoff,
 * invoke the explicit admission callback. A nonzero result stops without sending
 * or reporting a provider failure. Ordinary HTTP callers have no callback, so
 * an owner's HTTP revalidation request cannot recursively invoke this gate. */
int http_retry_post_guarded_bytes(const char *url, const char *auth_header, const void *body,
                                  size_t body_len, char **response_buf, int timeout_ms,
                                  const char *extra_headers, int max_attempts, int base_ms,
                                  int max_ms, const char *provider, const char *model,
                                  const char *session_id, http_retry_admit_cb_t admit_retry);

/* Explicit per-call observer; no process-global or thread-global mutable hook.
 * before runs after backoff on EVERY attempt and must durably admit the handoff.
 * after observes only actual transport returns; it cannot undo network effects. */
typedef struct
{
   void *context;
   int (*before)(void *context, const void *body, size_t length);
   void (*after)(void *context, int status, const char *response, size_t length);
} http_retry_observer_t;
int http_retry_post_observed_bytes(const char *url, const char *auth_header, const void *body,
                                   size_t body_len, char **response_buf, int timeout_ms,
                                   const char *extra_headers, int max_attempts, int base_ms,
                                   int max_ms, const char *provider, const char *model,
                                   const char *session_id, http_retry_admit_cb_t admit_retry,
                                   const http_retry_observer_t *observer);

/* Register a thread-local progress callback invoked after every model HTTP
 * attempt (decoupled from db1: the server side installs a callback that bumps the
 * running delegate job's heartbeat). This keeps a slow-but-progressing delegate —
 * any model whose run exceeds the stale-monitor idle threshold across multiple
 * turns — from being auto-cancelled as "stalled" (the reason only the fastest
 * model survived the fleet). Pass NULL to clear. */
typedef void (*http_progress_cb_t)(void);
void http_set_progress_cb(http_progress_cb_t cb);

#endif /* DEC_HTTP_RETRY_H */
