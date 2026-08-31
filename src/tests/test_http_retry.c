/* test_http_retry.c: unit tests for HTTP retry with exponential backoff */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "aimee.h"
#include "agent_exec.h"
#include "failover.h"
#include "http_retry.h"

static atomic_int g_request_cancelled;

int agent_request_cancelled(void)
{
   return atomic_load(&g_request_cancelled);
}

/* http_retry_post_context records failover events, and this binary satisfies
 * that at the call: tests/support/interaction_events_stub.c.
 *
 * It used to stub db1_conn to NULL instead, so the REAL recorder linked in and
 * found no connection to write through. That worked only while the store was
 * in-process. It is a bus client now, reaching a separate module, and there is
 * no handle to withhold -- a NULL db1_conn would not have made recording inert,
 * it would have left this test needing a running module to assert how backoff
 * clamps. */

/* --- progress callback (per-turn delegate heartbeat seam) --- */

static int g_progress_calls;
static void count_progress(void)
{
   g_progress_calls++;
}

/* The progress callback fires once per HTTP attempt while registered, and not at
 * all after it is cleared — the seam that lets a slow delegate bump its heartbeat
 * each turn so the stale-monitor doesn't cancel it. Uses an unreachable endpoint
 * so each attempt fails fast (connection refused) with a tiny backoff. */
static void test_progress_cb_fires_per_attempt(void)
{
   g_progress_calls = 0;
   http_set_progress_cb(count_progress);
   char *resp = NULL;
   (void)http_retry_post_context("http://127.0.0.1:1/x", NULL, "{}", &resp, 500, NULL, 2, 1, 1,
                                 "test", "test-model", NULL);
   free(resp);
   http_set_progress_cb(NULL);
   assert(g_progress_calls == 2); /* one per attempt */

   /* Cleared: no further callbacks. */
   g_progress_calls = 0;
   resp = NULL;
   (void)http_retry_post_context("http://127.0.0.1:1/x", NULL, "{}", &resp, 500, NULL, 1, 1, 1,
                                 "test", "test-model", NULL);
   free(resp);
   assert(g_progress_calls == 0);
   printf("  PASS: test_progress_cb_fires_per_attempt\n");
}

/* A budget-consuming STALL (an attempt that runs >= half its timeout waiting on a
 * stalled peer) caps retries to one extra attempt, so one flaky provider can't
 * spend Nx its timeout and drag a deadline-bounded roundtable panel. A listening
 * socket that never accepts/responds simulates the stall: connect + send succeed
 * (the kernel completes the handshake into the backlog) but the response read
 * blocks to the deadline. With max_attempts=3 the cap must reduce it to 2 actual
 * attempts. A FAST connection-refused is NOT capped (the progress test above uses
 * a refused port and still gets its full 2 attempts). */
static void test_stall_caps_retries(void)
{
   int srv = socket(AF_INET, SOCK_STREAM, 0);
   assert(srv >= 0);
   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0; /* ephemeral */
   assert(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(srv, 8) == 0);
   socklen_t alen = sizeof(addr);
   assert(getsockname(srv, (struct sockaddr *)&addr, &alen) == 0);
   int port = ntohs(addr.sin_port);

   char url[64];
   snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);

   g_progress_calls = 0;
   http_set_progress_cb(count_progress);
   char *resp = NULL;
   /* 200ms timeout, 3 attempts requested -> stall cap reduces to 2. */
   (void)http_retry_post_context(url, NULL, "{}", &resp, 200, NULL, 3, 1, 1, "test", "test-model",
                                 NULL);
   free(resp);
   http_set_progress_cb(NULL);
   close(srv);
   assert(g_progress_calls == 2); /* 3 requested, capped to 2 after the first stall */
   printf("  PASS: test_stall_caps_retries\n");
}

typedef struct
{
   int listener;
} cancel_server_t;

static void sleep_ms(int ms)
{
   struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
   nanosleep(&ts, NULL);
}

static int64_t monotonic_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *hold_response_until_after_cancel(void *arg)
{
   cancel_server_t *server = arg;
   int client = accept(server->listener, NULL, NULL);
   assert(client >= 0);
   char request[1024];
   assert(recv(client, request, sizeof(request), 0) > 0);
   sleep_ms(100);
   atomic_store(&g_request_cancelled, 1);
   /* Keep the peer open well beyond the client's expected return. If the HTTP
    * transport ignores cancellation, agent_http_post blocks here until close. */
   sleep_ms(1200);
   close(client);
   return NULL;
}

static void test_inflight_http_observes_parallel_cancel(void)
{
   int srv = socket(AF_INET, SOCK_STREAM, 0);
   assert(srv >= 0);
   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0;
   assert(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(srv, 1) == 0);
   socklen_t alen = sizeof(addr);
   assert(getsockname(srv, (struct sockaddr *)&addr, &alen) == 0);

   cancel_server_t server = {.listener = srv};
   pthread_t thread;
   atomic_store(&g_request_cancelled, 0);
   assert(pthread_create(&thread, NULL, hold_response_until_after_cancel, &server) == 0);

   char url[64];
   snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", ntohs(addr.sin_port));
   char *response = NULL;
   int64_t started = monotonic_ms();
   int status = agent_http_post(url, NULL, "{}", &response, 5000, NULL);
   int64_t elapsed = monotonic_ms() - started;
   free(response);

   assert(status < 0);
   assert(elapsed < 800); /* peer remains open for 1.3s; cancellation wins */
   pthread_join(thread, NULL);
   close(srv);
   atomic_store(&g_request_cancelled, 0);
   printf("  PASS: test_inflight_http_observes_parallel_cancel (%lldms)\n", (long long)elapsed);
}

static ssize_t read_full(int fd, void *buf, size_t len)
{
   size_t off = 0;
   while (off < len)
   {
      ssize_t n = read(fd, (unsigned char *)buf + off, len - off);
      if (n <= 0)
         return n;
      off += (size_t)n;
   }
   return (ssize_t)off;
}

static void capture_request_body(int client, int output)
{
   unsigned char request[4097];
   size_t used = 0;
   size_t header_len = 0;
   size_t body_len = 0;
   while (used < sizeof(request) - 1)
   {
      ssize_t n = recv(client, request + used, sizeof(request) - 1 - used, 0);
      assert(n > 0);
      used += (size_t)n;
      request[used] = '\0';
      for (size_t i = 3; i < used; i++)
         if (request[i - 3] == '\r' && request[i - 2] == '\n' && request[i - 1] == '\r' &&
             request[i] == '\n')
         {
            header_len = i + 1;
            break;
         }
      if (!header_len)
         continue;
      const char *content_length = strstr((const char *)request, "Content-Length:");
      assert(content_length != NULL);
      assert(sscanf(content_length, "Content-Length: %zu", &body_len) == 1);
      if (used >= header_len + body_len)
         break;
   }
   assert(header_len > 0 && used >= header_len + body_len);
   assert(body_len <= UINT32_MAX);
   uint32_t wire_len = (uint32_t)body_len;
   assert(write(output, &wire_len, sizeof(wire_len)) == (ssize_t)sizeof(wire_len));
   assert(write(output, request + header_len, body_len) == (ssize_t)body_len);
}

/* The bytes API must reuse the caller's exact pointer+length on every retry.
 * Embedded NUL makes an accidental strlen wrapper observable. */
static void test_exact_length_body_is_identical_across_retries(void)
{
   int srv = socket(AF_INET, SOCK_STREAM, 0);
   assert(srv >= 0);
   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0;
   assert(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   assert(listen(srv, 2) == 0);
   socklen_t alen = sizeof(addr);
   assert(getsockname(srv, (struct sockaddr *)&addr, &alen) == 0);

   int captured[2];
   assert(pipe(captured) == 0);
   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      close(captured[0]);
      for (int attempt = 0; attempt < 2; attempt++)
      {
         int client = accept(srv, NULL, NULL);
         assert(client >= 0);
         capture_request_body(client, captured[1]);
         const char *response = attempt == 0
                                    ? "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 2\r\n"
                                      "Connection: close\r\n\r\n{}"
                                    : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: "
                                      "close\r\n\r\n{}";
         assert(send(client, response, strlen(response), 0) == (ssize_t)strlen(response));
         close(client);
      }
      close(captured[1]);
      close(srv);
      _exit(0);
   }

   close(captured[1]);
   char url[64];
   snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", ntohs(addr.sin_port));
   const unsigned char expected[] = {'{', '"', 'x', '"', ':', '"', 'a', 0, 'b', '"', '}'};
   char *resp = NULL;
   assert(http_retry_post_context_bytes(url, NULL, expected, sizeof(expected), &resp, 1000, NULL, 2,
                                        1, 1, "test", "test-model", NULL) == 200);
   free(resp);
   close(srv);

   for (int attempt = 0; attempt < 2; attempt++)
   {
      uint32_t body_len = 0;
      unsigned char body[sizeof(expected)];
      assert(read_full(captured[0], &body_len, sizeof(body_len)) == (ssize_t)sizeof(body_len));
      assert(body_len == sizeof(expected));
      assert(read_full(captured[0], body, body_len) == (ssize_t)body_len);
      assert(memcmp(body, expected, sizeof(expected)) == 0);
   }
   close(captured[0]);
   int status = 0;
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   printf("  PASS: test_exact_length_body_is_identical_across_retries\n");
}

/* --- http_should_retry tests --- */

static void test_retryable_status_codes(void)
{
   /* Network errors are always retryable */
   assert(http_should_retry(-1) == 1);
   assert(http_should_retry(-999) == 1);

   /* Retryable HTTP status codes */
   assert(http_should_retry(408) == 1); /* Request Timeout */
   assert(http_should_retry(409) == 1); /* Conflict */
   assert(http_should_retry(429) == 1); /* Too Many Requests */
   assert(http_should_retry(500) == 1); /* Internal Server Error */
   assert(http_should_retry(502) == 1); /* Bad Gateway */
   assert(http_should_retry(503) == 1); /* Service Unavailable */
   assert(http_should_retry(504) == 1); /* Gateway Timeout */

   printf("  retryable status codes: ok\n");
}

static void test_non_retryable_status_codes(void)
{
   /* Success is not retryable */
   assert(http_should_retry(200) == 0);
   assert(http_should_retry(201) == 0);

   /* Client errors (non-retryable) */
   assert(http_should_retry(400) == 0); /* Bad Request */
   assert(http_should_retry(401) == 0); /* Unauthorized */
   assert(http_should_retry(403) == 0); /* Forbidden */
   assert(http_should_retry(404) == 0); /* Not Found */
   assert(http_should_retry(422) == 0); /* Unprocessable Entity */

   /* Other server errors not in the retryable set */
   assert(http_should_retry(501) == 0); /* Not Implemented */

   /* Zero (no response) is not retryable */
   assert(http_should_retry(0) == 0);

   printf("  non-retryable status codes: ok\n");
}

/* --- http_backoff_ms tests --- */

static void test_backoff_basic(void)
{
   /* Attempt 0: base delay */
   assert(http_backoff_ms(0, 1000, 30000) == 1000);

   /* Attempt 1: 2x base */
   assert(http_backoff_ms(1, 1000, 30000) == 2000);

   /* Attempt 2: 4x base */
   assert(http_backoff_ms(2, 1000, 30000) == 4000);

   /* Attempt 3: 8x base */
   assert(http_backoff_ms(3, 1000, 30000) == 8000);

   /* Attempt 4: 16x base */
   assert(http_backoff_ms(4, 1000, 30000) == 16000);

   printf("  backoff basic doubling: ok\n");
}

static void test_backoff_clamped(void)
{
   /* Should not exceed max_ms */
   assert(http_backoff_ms(5, 1000, 30000) == 30000);
   assert(http_backoff_ms(10, 1000, 30000) == 30000);
   assert(http_backoff_ms(100, 1000, 30000) == 30000);

   /* Small max_ms */
   assert(http_backoff_ms(0, 1000, 500) == 500);
   assert(http_backoff_ms(3, 100, 500) == 500);

   printf("  backoff clamped to max: ok\n");
}

static void test_backoff_overflow_safe(void)
{
   /* Very high attempt count should not overflow */
   int result = http_backoff_ms(1000, 1000, 30000);
   assert(result == 30000);
   assert(result > 0); /* no negative overflow */

   /* Maximum int-safe attempt */
   result = http_backoff_ms(2147483647, 1000, 30000);
   assert(result == 30000);
   assert(result > 0);

   printf("  backoff overflow safety: ok\n");
}

static void test_backoff_edge_cases(void)
{
   /* Negative attempt treated as 0 */
   assert(http_backoff_ms(-1, 1000, 30000) == 1000);
   assert(http_backoff_ms(-100, 1000, 30000) == 1000);

   /* Zero/negative base_ms uses default */
   int result = http_backoff_ms(0, 0, 30000);
   assert(result == HTTP_RETRY_BASE_MS);

   /* Zero/negative max_ms uses default */
   result = http_backoff_ms(0, 1000, 0);
   assert(result == 1000); /* base < default max */

   printf("  backoff edge cases: ok\n");
}

static void test_backoff_small_values(void)
{
   /* Base of 1ms */
   assert(http_backoff_ms(0, 1, 100) == 1);
   assert(http_backoff_ms(1, 1, 100) == 2);
   assert(http_backoff_ms(5, 1, 100) == 32);
   assert(http_backoff_ms(7, 1, 100) == 100); /* clamped */

   printf("  backoff small values: ok\n");
}

static void test_model_loading_detection(void)
{
   assert(http_response_is_model_loading("{\"error\":{\"message\":\"Loading model\"}}") == 1);
   assert(http_response_is_model_loading("Model is loading") == 1);
   assert(http_response_is_model_loading("provider says model is loading") == 1);
   assert(http_response_is_model_loading("{\"error\":\"server error\"}") == 0);
   assert(http_response_is_model_loading(NULL) == 0);

   printf("  model loading detection: ok\n");
}

static void test_failover_status_classification(void)
{
   assert(failover_classify(NULL, -1, NULL) == FAILOVER_TIMEOUT);
   assert(failover_classify(NULL, 402, "{\"error\":\"out of credits\"}") == FAILOVER_BILLING);
   assert(failover_classify(NULL, 429, "{\"error\":\"rate limit\"}") == FAILOVER_RATE_LIMIT);
   assert(failover_classify(NULL, 404, "{\"error\":\"model not found\"}") ==
          FAILOVER_MODEL_NOT_FOUND);
   assert(failover_classify(NULL, 413, "{\"error\":\"payload too large\"}") ==
          FAILOVER_PAYLOAD_TOO_LARGE);
   assert(failover_classify(NULL, 500, NULL) == FAILOVER_SERVER_ERROR);
   assert(failover_classify(NULL, 503, "{\"error\":\"Loading model\"}") == FAILOVER_OVERLOADED);

   printf("  failover status classification: ok\n");
}

static void test_failover_priority_and_actions(void)
{
   failover_reason_t reason =
       failover_classify(NULL, 400, "{\"error\":\"context length exceeded\"}");
   assert(reason == FAILOVER_CONTEXT_OVERFLOW);
   assert(failover_action(reason) == RECOVER_COMPRESS_CONTEXT);

   reason = failover_classify(NULL, 400, "{\"error\":\"expected a string but got array\"}");
   assert(reason == FAILOVER_CONTENT_SHAPE);
   assert(failover_action(reason) == RECOVER_DOWNGRADE_CONTENT);

   reason = failover_classify(NULL, 400, "{\"error\":\"malformed request\"}");
   assert(reason == FAILOVER_FORMAT_ERROR);
   assert(failover_action(reason) == RECOVER_ABORT);

   assert(failover_action(FAILOVER_BILLING) == RECOVER_ROTATE_CREDENTIAL);
   assert(failover_action(FAILOVER_RATE_LIMIT) == RECOVER_BACKOFF);
   assert(failover_action(FAILOVER_MODEL_NOT_FOUND) == RECOVER_FALLBACK_MODEL);
   assert(strcmp(failover_reason_name(FAILOVER_CONTEXT_OVERFLOW), "context_overflow") == 0);
   assert(strcmp(failover_action_name(RECOVER_DOWNGRADE_CONTENT), "downgrade_content") == 0);

   printf("  failover priority and actions: ok\n");
}

static void test_provider_specific_failover_classification(void)
{
   failover_reason_t reason = failover_classify(
       "openrouter", 404, "{\"error\":\"No endpoints found matching your data policy\"}");
   assert(reason == FAILOVER_PROVIDER_POLICY);
   assert(failover_action(reason) == RECOVER_FALLBACK_PROVIDER);

   reason = failover_classify("ollama", 500,
                              "{\"error\":\"num_ctx exceeded; prompt too long for model\"}");
   assert(reason == FAILOVER_CONTEXT_OVERFLOW);
   assert(failover_action(reason) == RECOVER_COMPRESS_CONTEXT);

   printf("  provider-specific failover classification: ok\n");
}

int main(void)
{
   printf("test_http_retry:\n");

   test_retryable_status_codes();
   test_non_retryable_status_codes();
   test_backoff_basic();
   test_backoff_clamped();
   test_backoff_overflow_safe();
   test_backoff_edge_cases();
   test_backoff_small_values();
   test_model_loading_detection();
   test_failover_status_classification();
   test_failover_priority_and_actions();
   test_provider_specific_failover_classification();
   test_progress_cb_fires_per_attempt();
   test_stall_caps_retries();
   test_exact_length_body_is_identical_across_retries();
   test_inflight_http_observes_parallel_cancel();

   printf("all http_retry tests passed.\n");
   return 0;
}
