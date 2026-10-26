/* provider_client.c: shared LLM-provider client. See provider_client.h.
 *
 * Built on agent_http_post (linked by aimee-server and aimee-kb alike) with a
 * self-contained retry loop, so the kb need not link the server-only,
 * DB1-coupled http_retry/failover modules. The retryable-status set mirrors
 * http_retry's contract (408/409/429/5xx + network errors). */

#include "aimee.h" /* MAX_PATH_LEN etc., pulled in before agent_types.h */
#include "provider_client.h"

#include "agent_exec.h" /* agent_http_post */
#include "cJSON.h"
#include <aimee/translation/aimee_backend.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- request building --- */

static char *provider_build_ir(const provider_def_t *def, const aimee_request_t *request)
{
   if (!def || !request || aimee_ir_request_admit(request))
      return NULL;
   aimee_request_t profile = *request; /* borrowed view; never free caller storage */
   if (def->model && def->model[0])
      profile.model = (char *)def->model;
   if (def->temperature >= 0)
   {
      profile.temperature = def->temperature;
      profile.has_temperature = 1;
   }
   if (def->max_tokens > 0)
   {
      profile.max_tokens = def->max_tokens;
      profile.has_max_tokens = 1;
   }
   return openai_backend_render_local(&profile, def->disable_thinking);
}
/* --- response parsing --- */

int provider_client_parse_openai(const char *body, provider_completion_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!body)
      return -1;
   char error[128];
   int rc = aimee_backend_parse_wire(body, strlen(body), AIMEE_WIRE_OPENAI_CHAT, &out->ir, error,
                                     sizeof error);
   if (rc || aimee_ir_response_admit(&out->ir) || out->ir.error_message ||
       out->ir.stop_reason == AIMEE_STOP_CONTENT_FILTER || out->ir.stop_reason == AIMEE_STOP_ERROR)
      return -1;
   /* Admission keeps the complete response. The application must choose a
    * permitted terminal state before projecting authored TEXT into its domain. */
   int authored_text = 0;
   for (int i = 0; i < out->ir.n_content; i++)
      authored_text |= out->ir.content[i].type == AIMEE_BLK_TEXT && out->ir.content[i].text &&
                       out->ir.content[i].text[0];
   if (!authored_text)
      return -1;
   return 0;
}

/* --- transport + retry --- */

/* Mirrors http_retry's retryable set: transient server/limit statuses and any
 * network-level error (status < 0). Everything else (2xx, 4xx auth/not-found)
 * fails fast. */
static int wire_is_retryable(int http_status)
{
   if (http_status < 0)
      return 1;
   switch (http_status)
   {
   case 408:
   case 409:
   case 429:
   case 500:
   case 502:
   case 503:
   case 504:
      return 1;
   default:
      return 0;
   }
}

/* Exponential backoff (base * 2^attempt) clamped to max; overflow-safe. */
static int wire_backoff_ms(int attempt, int base_ms, int max_ms)
{
   long d = base_ms;
   for (int i = 0; i < attempt; i++)
   {
      /* Check before doubling so `d` never exceeds max_ms (an int) — no overflow
       * regardless of attempt count or max_ms magnitude. */
      if (d >= max_ms || d > max_ms / 2)
         return max_ms;
      d *= 2;
   }
   return d > max_ms ? max_ms : (int)d;
}

int provider_client_complete_ir(const provider_def_t *def, const aimee_request_t *request,
                                provider_completion_t *out, char *errbuf, size_t errlen)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (errbuf && errlen)
      errbuf[0] = '\0';
   if (!def || !def->base_url || !def->base_url[0] || !request || !out)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "invalid arguments");
      return -1;
   }
   if (def->wire != PROVIDER_WIRE_OPENAI_CHAT)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "unsupported wire format %d", (int)def->wire);
      return -1;
   }

   char *body = provider_build_ir(def, request);
   if (!body)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "failed to build request");
      return -1;
   }
   /* URL = base_url + "/chat/completions" (tolerate a trailing slash). snprintf
    * is bounded (no overflow), but a truncated URL/Bearer would silently hit the
    * wrong endpoint or 401 with valid creds, so treat truncation as an error. */
   size_t bl = strlen(def->base_url);
   int has_slash = bl > 0 && def->base_url[bl - 1] == '/';
   char url[2048];
   int un = snprintf(url, sizeof(url), "%s%schat/completions", def->base_url, has_slash ? "" : "/");
   if (un < 0 || (size_t)un >= sizeof(url))
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "base_url too long");
      free(body);
      return -1;
   }

   char auth[1280];
   const char *auth_header = NULL;
   if (def->api_key && def->api_key[0])
   {
      int an = snprintf(auth, sizeof(auth), "Authorization: Bearer %s", def->api_key);
      if (an < 0 || (size_t)an >= sizeof(auth))
      {
         if (errbuf && errlen)
            snprintf(errbuf, errlen, "api_key too long");
         free(body);
         return -1;
      }
      auth_header = auth;
   }

   int timeout = def->timeout_ms > 0 ? def->timeout_ms : PROVIDER_CLIENT_DEFAULT_TIMEOUT_MS;
   int attempts = def->max_attempts > 0 ? def->max_attempts : PROVIDER_CLIENT_DEFAULT_ATTEMPTS;

   int status = -1;
   char *resp = NULL;
   for (int a = 0; a < attempts; a++)
   {
      free(resp);
      resp = NULL;
      status = agent_http_post(url, auth_header, body, &resp, timeout, NULL);
      if (!wire_is_retryable(status))
         break;
      if (a + 1 < attempts)
      {
         int ms = wire_backoff_ms(a, 1000, 30000);
         struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
         nanosleep(&ts, NULL);
      }
   }
   free(body);

   if (status < 200 || status >= 300)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "provider HTTP %d", status);
      free(resp);
      out->http_status = status;
      return -1;
   }

   int rc = provider_client_parse_openai(resp, out); /* zeroes out, then fills */
   free(resp);
   out->http_status = status;
   if (rc != 0 && errbuf && errlen)
      snprintf(errbuf, errlen, "%s",
               out->ir.error_message ? out->ir.error_message
                                     : "provider response failed canonical admission");
   return rc;
}

void provider_completion_free(provider_completion_t *out)
{
   if (!out)
      return;
   aimee_response_free(&out->ir);
}
