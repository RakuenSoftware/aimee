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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- request building --- */

struct cJSON *provider_client_build_openai(const provider_def_t *def, struct cJSON *messages,
                                           struct cJSON *json_schema)
{
   if (!def || !messages)
      return NULL;
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return NULL;
   if (def->model && def->model[0])
      cJSON_AddStringToObject(req, "model", def->model);

   /* Deep-copy the caller's messages so ownership stays with them. */
   cJSON *msgs = cJSON_Duplicate(messages, 1);
   if (!msgs)
   {
      cJSON_Delete(req);
      return NULL;
   }
   cJSON_AddItemToObject(req, "messages", msgs);

   if (def->temperature >= 0.0)
      cJSON_AddNumberToObject(req, "temperature", def->temperature);
   if (def->max_tokens > 0)
      cJSON_AddNumberToObject(req, "max_tokens", def->max_tokens);

   /* Skip the model's reasoning pass for mechanical stages (see provider_def_t
    * .disable_thinking). Sent as the jinja chat-template kwarg understood by
    * llama.cpp (--jinja) / vLLM; other endpoints ignore the extra field. */
   if (def->disable_thinking)
   {
      cJSON *ctk = cJSON_CreateObject();
      if (ctk)
      {
         cJSON_AddBoolToObject(ctk, "enable_thinking", 0);
         cJSON_AddItemToObject(req, "chat_template_kwargs", ctk);
      }
   }

   if (json_schema)
   {
      cJSON *schema_copy = cJSON_Duplicate(json_schema, 1);
      if (!schema_copy)
      {
         cJSON_Delete(req);
         return NULL;
      }
      cJSON *rf = cJSON_CreateObject();
      cJSON *js = cJSON_CreateObject();
      if (!rf || !js)
      {
         cJSON_Delete(schema_copy);
         cJSON_Delete(rf);
         cJSON_Delete(js);
         cJSON_Delete(req);
         return NULL;
      }
      cJSON_AddStringToObject(rf, "type", "json_schema");
      cJSON_AddStringToObject(js, "name", "curator_output");
      cJSON_AddItemToObject(js, "schema", schema_copy);
      cJSON_AddBoolToObject(js, "strict", 1);
      cJSON_AddItemToObject(rf, "json_schema", js);
      cJSON_AddItemToObject(req, "response_format", rf);
   }
   return req;
}

/* --- response parsing --- */

int provider_client_parse_openai(const char *body, provider_completion_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!body || !out)
      return -1;

   cJSON *root = cJSON_Parse(body);
   if (!root)
      return -1;

   int rc = -1;
   cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
   cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
   if (choice0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(choice0, "message");
      cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
      if (cJSON_IsString(content) && content->valuestring)
      {
         out->content = strdup(content->valuestring);
         if (out->content)
            rc = 0;
      }
      cJSON *fr = cJSON_GetObjectItemCaseSensitive(choice0, "finish_reason");
      if (cJSON_IsString(fr) && fr->valuestring)
         snprintf(out->finish_reason, sizeof(out->finish_reason), "%s", fr->valuestring);
   }

   cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
   if (usage)
   {
      cJSON *pt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
      cJSON *ct = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
      if (cJSON_IsNumber(pt))
         out->prompt_tokens = pt->valueint;
      if (cJSON_IsNumber(ct))
         out->completion_tokens = ct->valueint;
   }

   cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
   if (cJSON_IsString(model) && model->valuestring)
      snprintf(out->model, sizeof(out->model), "%s", model->valuestring);

   cJSON_Delete(root);
   return rc;
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

int provider_client_complete(const provider_def_t *def, struct cJSON *messages,
                             struct cJSON *json_schema, provider_completion_t *out, char *errbuf,
                             size_t errlen)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (errbuf && errlen)
      errbuf[0] = '\0';
   if (!def || !def->base_url || !def->base_url[0] || !messages || !out)
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

   cJSON *req = provider_client_build_openai(def, messages, json_schema);
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
      snprintf(errbuf, errlen, "unparseable provider response");
   return rc;
}

void provider_completion_free(provider_completion_t *out)
{
   if (!out)
      return;
   free(out->content);
   out->content = NULL;
}
