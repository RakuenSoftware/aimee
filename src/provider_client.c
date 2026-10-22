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
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include <limits.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- request building --- */

static int provider_import_request(cJSON *messages, cJSON *schema, aimee_request_t *out)
{
   cJSON *native = cJSON_CreateObject();
   if (!native || !cJSON_AddItemToObject(native, "messages", cJSON_Duplicate(messages, 1)))
   {
      cJSON_Delete(native);
      return -1;
   }
   char error[128];
   int rc = openai_frontend_parse(native, out, error, sizeof error);
   cJSON_Delete(native);
   if (rc)
      return -1;
   if (schema)
   {
      out->output_format = (aimee_output_format_t){.kind = AIMEE_OUTPUT_JSON_SCHEMA,
                                                   .name = strdup("curator_output"),
                                                   .schema = cJSON_Duplicate(schema, 1),
                                                   .strict = AIMEE_STRICT_TRUE};
      if (!out->output_format.name || !out->output_format.schema)
      {
         aimee_request_free(out);
         return -1;
      }
   }
   return 0;
}
static cJSON *provider_build_ir(const provider_def_t *def, const aimee_request_t *request)
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
   cJSON *native = openai_backend_build(&profile);
   if (native && def->disable_thinking)
   {
      /* A provider-profile extension, added only at the native output boundary. */
      cJSON *kwargs = cJSON_AddObjectToObject(native, "chat_template_kwargs");
      if (!kwargs || !cJSON_AddBoolToObject(kwargs, "enable_thinking", 0))
      {
         cJSON_Delete(native);
         return NULL;
      }
   }
   return native;
}
struct cJSON *provider_client_build_openai(const provider_def_t *def, struct cJSON *messages,
                                           struct cJSON *json_schema)
{
   if (!def || !cJSON_IsArray(messages))
      return NULL;
   aimee_request_t request = {0};
   if (provider_import_request(messages, json_schema, &request))
      return NULL;
   cJSON *native = provider_build_ir(def, &request);
   aimee_request_free(&request);
   return native;
}

/* --- response parsing --- */

static char *provider_reasoning_text(const aimee_response_t *response)
{
   size_t length = 0;
   int found = 0;
   for (int i = 0; i < response->n_content; i++)
      if (response->content[i].type == AIMEE_BLK_THINKING && response->content[i].text)
      {
         length += strlen(response->content[i].text);
         found = 1;
      }
   if (!found)
      return NULL;
   char *text = calloc(length + 1, 1);
   if (text)
      for (int i = 0; i < response->n_content; i++)
         if (response->content[i].type == AIMEE_BLK_THINKING && response->content[i].text)
            strcat(text, response->content[i].text);
   return text;
}
int provider_client_parse_openai(const char *body, provider_completion_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!body)
      return -1;
   const char *end = NULL;
   size_t length = strlen(body);
   cJSON *root = cJSON_ParseWithLengthOpts(body, length, &end, 0);
   if (end)
      while (end < body + length && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t'))
         end++;
   const cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
   const char *finish =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(choice, "finish_reason"));
   char error[128];
   const cJSON *failure = cJSON_GetObjectItemCaseSensitive(root, "error");
   int rc = root && end == body + length &&
                    ((finish && finish[0]) || (failure && !cJSON_IsNull(failure)))
                ? openai_backend_parse(root, &out->ir, error, sizeof error)
                : -1;
   cJSON_Delete(root);
   if (rc || aimee_ir_response_admit(&out->ir) || out->ir.error_message)
      return -1;
   int textual = 0;
   for (int i = 0; i < out->ir.n_content; i++)
      textual |= out->ir.content[i].type == AIMEE_BLK_TEXT ||
                 out->ir.content[i].type == AIMEE_BLK_THINKING;
   if (!textual)
      return -1;
   out->content = aimee_ir_response_text_alloc(&out->ir);
   out->reasoning = provider_reasoning_text(&out->ir);
   /* Curator JSON recovery may inspect a reasoning-only local-model reply.
    * This application view does not reclassify the canonical THINKING block. */
   if ((!out->content || !out->content[0]) && out->reasoning && out->reasoning[0])
   {
      free(out->content);
      out->content = strdup(out->reasoning);
   }
   if (!out->content)
      return -1;
   out->prompt_tokens = out->ir.usage_in > INT_MAX ? INT_MAX : (int)out->ir.usage_in;
   out->completion_tokens = out->ir.usage_out > INT_MAX ? INT_MAX : (int)out->ir.usage_out;
   snprintf(out->model, sizeof out->model, "%s", out->ir.model ? out->ir.model : "");
   snprintf(out->finish_reason, sizeof out->finish_reason, "%s",
            out->ir.raw_stop_reason ? out->ir.raw_stop_reason : "");
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

   cJSON *req = provider_build_ir(def, request);
   if (!req)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "failed to build request");
      return -1;
   }
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "failed to serialize request");
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

int provider_client_complete(const provider_def_t *def, struct cJSON *messages,
                             struct cJSON *json_schema, provider_completion_t *out, char *errbuf,
                             size_t errlen)
{
   if (out)
      memset(out, 0, sizeof *out);
   aimee_request_t request = {0};
   if (!out || !cJSON_IsArray(messages) || provider_import_request(messages, json_schema, &request))
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "invalid provider request");
      return -1;
   }
   int rc = provider_client_complete_ir(def, &request, out, errbuf, errlen);
   aimee_request_free(&request);
   return rc;
}

void provider_completion_free(provider_completion_t *out)
{
   if (!out)
      return;
   aimee_response_free(&out->ir);
   free(out->content);
   out->content = NULL;
   free(out->reasoning);
   out->reasoning = NULL;
}
