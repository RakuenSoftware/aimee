/* test_http_retry.c: unit tests for HTTP retry with exponential backoff */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "failover.h"
#include "http_retry.h"

/* http_retry_post_context records failover events via interaction_events.o ->
 * db1_conn; this test binary doesn't link db1. Stub it to NULL so recording
 * no-ops (the real path guards on a NULL connection). */
void *db1_conn(void)
{
   return NULL;
}

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

   printf("all http_retry tests passed.\n");
   return 0;
}
