/* http_retry.c: exponential backoff with overflow-safe retries for API calls */
#include "aimee.h"
#include "agent_exec.h"
#include "failover.h"
#include "http_retry.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Thread-local progress callback (see http_retry.h). Each connection/turn runs on
 * its own worker thread, so a thread-local matches the delegate run's lifetime. */
static _Thread_local http_progress_cb_t g_progress_cb;

void http_set_progress_cb(http_progress_cb_t cb)
{
   g_progress_cb = cb;
}

int http_should_retry(int http_status)
{
   failover_reason_t reason = failover_classify(NULL, http_status, NULL);
   return reason != FAILOVER_UNKNOWN && failover_reason_uses_backoff(reason);
}

int http_response_is_model_loading(const char *response_body)
{
   if (!response_body || !response_body[0])
      return 0;

   return strstr(response_body, "Loading model") != NULL ||
          strstr(response_body, "Model is loading") != NULL ||
          strstr(response_body, "Model loading") != NULL ||
          strstr(response_body, "loading model") != NULL ||
          strstr(response_body, "model is loading") != NULL ||
          strstr(response_body, "model loading") != NULL;
}

int http_backoff_ms(int attempt, int base_ms, int max_ms)
{
   if (base_ms <= 0)
      base_ms = HTTP_RETRY_BASE_MS;
   if (max_ms <= 0)
      max_ms = HTTP_RETRY_MAX_MS;
   if (attempt < 0)
      attempt = 0;

   int delay = base_ms;
   for (int i = 0; i < attempt; i++)
   {
      /* Overflow-safe doubling: stop if we'd exceed max_ms */
      if (delay > max_ms / 2)
      {
         delay = max_ms;
         break;
      }
      delay *= 2;
   }

   if (delay > max_ms)
      delay = max_ms;

   return delay;
}

int http_retry_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers, int max_attempts, int base_ms,
                    int max_ms)
{
   return http_retry_post_context(url, auth_header, body, response_buf, timeout_ms, extra_headers,
                                  max_attempts, base_ms, max_ms, NULL, NULL, NULL);
}

/* Providers with stricter rate limits need longer backoff windows than the
 * generic defaults.  When a 429 is detected on the first attempt, upgrade the
 * params to the provider-specific floor so we don't bail before the window
 * clears.  Only applies when the caller passes the default values (i.e. when
 * it hasn't already supplied provider-specific overrides). */
static void apply_provider_rate_limit_params(const char *provider, int http_status,
                                             int *max_attempts, int *base_ms, int *max_ms)
{
   if (!provider || http_status != 429)
      return;
   if (strcmp(provider, "mistral") == 0)
   {
      if (*max_attempts < 6)
         *max_attempts = 6;
      if (*base_ms < 3000)
         *base_ms = 3000;
      if (*max_ms < 60000)
         *max_ms = 60000;
   }
}

int http_retry_post_context(const char *url, const char *auth_header, const char *body,
                            char **response_buf, int timeout_ms, const char *extra_headers,
                            int max_attempts, int base_ms, int max_ms, const char *provider,
                            const char *model, const char *session_id)
{
   if (max_attempts <= 0)
      max_attempts = 1; /* at least one attempt */

   int http_status = -1;
   int effective_max_attempts = max_attempts;

   for (int attempt = 0; attempt < effective_max_attempts; attempt++)
   {
      /* Sleep before retry (not before first attempt) */
      if (attempt > 0)
      {
         int delay = http_backoff_ms(attempt - 1, base_ms, max_ms);
         aimee_log(LOG_INFO, "http_retry", "attempt %d/%d, retrying in %dms (status %d)...",
                   attempt + 1, effective_max_attempts, delay, http_status);
         usleep((unsigned)(delay * 1000));
      }

      /* Free previous response if retrying */
      if (*response_buf)
      {
         free(*response_buf);
         *response_buf = NULL;
      }

      aimee_log(LOG_INFO, "http_retry",
                "attempt %d/%d: POST %s (provider=%s model=%s timeout=%dms)", attempt + 1,
                effective_max_attempts, url ? url : "?", provider ? provider : "?",
                model ? model : "?", timeout_ms);

      http_status =
          agent_http_post(url, auth_header, body, response_buf, timeout_ms, extra_headers);

      aimee_log(LOG_INFO, "http_retry", "attempt %d/%d: HTTP %d (provider=%s model=%s)",
                attempt + 1, effective_max_attempts, http_status, provider ? provider : "?",
                model ? model : "?");

      /* A completed model attempt is forward progress: refresh the running
       * delegate's heartbeat so a slow-but-progressing job (any model slower than
       * the stale-monitor idle threshold) is not auto-cancelled as stalled. */
      if (g_progress_cb)
         g_progress_cb();

      failover_reason_t reason =
          failover_classify(provider, http_status, response_buf ? *response_buf : NULL);
      if (reason != FAILOVER_NONE)
      {
         recover_action_t action = failover_action(reason);
         aimee_log(LOG_DEBUG, "http_retry", "classified status %d as %s -> %s", http_status,
                   failover_reason_name(reason), failover_action_name(action));
         (void)failover_record_event(session_id, provider, model, http_status, reason, action,
                                     attempt + 1, 0);
      }

      if (http_status == 503 && response_buf && http_response_is_model_loading(*response_buf) &&
          effective_max_attempts < HTTP_RETRY_MODEL_LOADING_MAX_ATTEMPTS)
      {
         effective_max_attempts = HTTP_RETRY_MODEL_LOADING_MAX_ATTEMPTS;
         aimee_log(LOG_INFO, "http_retry",
                   "provider reports model loading; extending retry budget to %d attempts",
                   effective_max_attempts);
      }

      /* On first 429, upgrade retry budget for providers with strict rate limits. */
      if (http_status == 429 && attempt == 0)
      {
         apply_provider_rate_limit_params(provider, http_status, &effective_max_attempts, &base_ms,
                                          &max_ms);
         if (effective_max_attempts > max_attempts)
            aimee_log(LOG_INFO, "http_retry",
                      "provider '%s' rate limited; extending retry budget to %d attempts "
                      "(base=%dms, max=%dms)",
                      provider ? provider : "unknown", effective_max_attempts, base_ms, max_ms);
      }

      /* Success or non-retryable error: return immediately */
      if (!http_should_retry(http_status))
         return http_status;
   }

   /* Exhausted all retries */
   return http_status;
}
