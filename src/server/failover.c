/* failover.c: typed API failure classification and recovery action mapping. */

#include "failover.h"
#include "util.h"
#include "cJSON.h"
#include "modules/db1/interaction_events.h"
#include "model_provider.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int body_has_any(const char *body, const char *const *markers)
{
   if (!body || !body[0])
      return 0;
   for (int i = 0; markers[i]; i++)
   {
      if (str_contains_ci(body, markers[i]))
         return 1;
   }
   return 0;
}

static int body_is_context_overflow(const char *body)
{
   static const char *const markers[] = {
       "context length exceeded", "maximum context length", "context window",     "context limit",
       "too many tokens",         "token limit exceeded",   "prompt is too long", NULL,
   };
   return body_has_any(body, markers);
}

static int body_is_billing(const char *body)
{
   static const char *const markers[] = {
       "insufficient quota", "credit balance", "billing", "payment required",
       "quota exceeded",     "out of credits", NULL,
   };
   return body_has_any(body, markers);
}

static int body_is_model_missing(const char *body)
{
   static const char *const markers[] = {
       "model not found", "invalid model", "unknown model", "model does not exist", NULL,
   };
   return body_has_any(body, markers);
}

static int body_is_content_shape(const char *body)
{
   static const char *const markers[] = {
       "image content not supported",
       "multimodal not supported",
       "expected a string",
       "expected string",
       "got array",
       "content must be a string",
       "unsupported content type",
       "list-valued",
       NULL,
   };
   return body_has_any(body, markers);
}

static int body_is_policy_block(const char *body)
{
   static const char *const markers[] = {
       "provider policy", "policy violation", "blocked by", "data policy", "privacy policy", NULL,
   };
   return body_has_any(body, markers);
}

static int body_is_format_error(const char *body)
{
   static const char *const markers[] = {
       "malformed",   "invalid request",  "invalid json", "schema",
       "bad request", "missing required", NULL,
   };
   return body_has_any(body, markers);
}

failover_reason_t failover_classify(const char *provider, int http_status, const char *body)
{
   if (http_status >= 200 && http_status < 400)
      return FAILOVER_NONE;
   if (http_status < 0 || http_status == 408 || http_status == 504)
      return FAILOVER_TIMEOUT;

   if (provider && provider[0])
   {
      model_provider_t *profile = model_provider_get(provider);
      failover_reason_t provider_reason = FAILOVER_NONE;
      if (profile && profile->classify_body &&
          profile->classify_body(profile, http_status, body, &provider_reason))
      {
         return provider_reason;
      }
   }

   if (body_is_billing(body) || http_status == 402)
      return FAILOVER_BILLING;
   if (body_is_context_overflow(body))
      return FAILOVER_CONTEXT_OVERFLOW;
   if (body_is_model_missing(body) || http_status == 404)
      return FAILOVER_MODEL_NOT_FOUND;
   if (body_is_content_shape(body))
      return FAILOVER_CONTENT_SHAPE;
   if (body_is_policy_block(body))
      return FAILOVER_PROVIDER_POLICY;

   if (http_status == 401)
      return FAILOVER_AUTH;
   if (http_status == 403)
      return FAILOVER_AUTH_PERMANENT;
   if (http_status == 429)
      return FAILOVER_RATE_LIMIT;
   if (http_status == 413)
      return FAILOVER_PAYLOAD_TOO_LARGE;
   if (http_status == 409 || http_status == 500 || http_status == 502)
      return FAILOVER_SERVER_ERROR;
   if (http_status == 503 || http_status == 529)
      return FAILOVER_OVERLOADED;
   if (http_status == 400 || http_status == 422 || body_is_format_error(body))
      return FAILOVER_FORMAT_ERROR;

   return FAILOVER_UNKNOWN;
}

recover_action_t failover_action(failover_reason_t reason)
{
   switch (reason)
   {
   case FAILOVER_NONE:
      return RECOVER_NONE;
   case FAILOVER_AUTH:
   case FAILOVER_BILLING:
      return RECOVER_ROTATE_CREDENTIAL;
   case FAILOVER_RATE_LIMIT:
   case FAILOVER_OVERLOADED:
   case FAILOVER_SERVER_ERROR:
   case FAILOVER_TIMEOUT:
   case FAILOVER_UNKNOWN:
      return RECOVER_BACKOFF;
   case FAILOVER_CONTEXT_OVERFLOW:
      return RECOVER_COMPRESS_CONTEXT;
   case FAILOVER_PAYLOAD_TOO_LARGE:
      return RECOVER_SHRINK_PAYLOAD;
   case FAILOVER_MODEL_NOT_FOUND:
      return RECOVER_FALLBACK_MODEL;
   case FAILOVER_PROVIDER_POLICY:
      return RECOVER_FALLBACK_PROVIDER;
   case FAILOVER_CONTENT_SHAPE:
      return RECOVER_DOWNGRADE_CONTENT;
   case FAILOVER_AUTH_PERMANENT:
   case FAILOVER_FORMAT_ERROR:
      return RECOVER_ABORT;
   }
   return RECOVER_ABORT;
}

int failover_reason_uses_backoff(failover_reason_t reason)
{
   return failover_action(reason) == RECOVER_BACKOFF;
}

const char *failover_reason_name(failover_reason_t reason)
{
   switch (reason)
   {
   case FAILOVER_NONE:
      return "none";
   case FAILOVER_AUTH:
      return "auth";
   case FAILOVER_AUTH_PERMANENT:
      return "auth_permanent";
   case FAILOVER_BILLING:
      return "billing";
   case FAILOVER_RATE_LIMIT:
      return "rate_limit";
   case FAILOVER_OVERLOADED:
      return "overloaded";
   case FAILOVER_SERVER_ERROR:
      return "server_error";
   case FAILOVER_TIMEOUT:
      return "timeout";
   case FAILOVER_CONTEXT_OVERFLOW:
      return "context_overflow";
   case FAILOVER_PAYLOAD_TOO_LARGE:
      return "payload_too_large";
   case FAILOVER_MODEL_NOT_FOUND:
      return "model_not_found";
   case FAILOVER_PROVIDER_POLICY:
      return "provider_policy";
   case FAILOVER_FORMAT_ERROR:
      return "format_error";
   case FAILOVER_CONTENT_SHAPE:
      return "content_shape";
   case FAILOVER_UNKNOWN:
      return "unknown";
   }
   return "unknown";
}

const char *failover_action_name(recover_action_t action)
{
   switch (action)
   {
   case RECOVER_NONE:
      return "none";
   case RECOVER_BACKOFF:
      return "backoff";
   case RECOVER_ROTATE_CREDENTIAL:
      return "rotate_credential";
   case RECOVER_FALLBACK_PROVIDER:
      return "fallback_provider";
   case RECOVER_FALLBACK_MODEL:
      return "fallback_model";
   case RECOVER_COMPRESS_CONTEXT:
      return "compress_context";
   case RECOVER_SHRINK_PAYLOAD:
      return "shrink_payload";
   case RECOVER_DOWNGRADE_CONTENT:
      return "downgrade_content";
   case RECOVER_ABORT:
      return "abort";
   }
   return "abort";
}

int failover_record_event(const char *session_id, const char *provider, const char *model,
                          int http_status, failover_reason_t reason, recover_action_t action,
                          int attempt, int recovered)
{
   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return -1;

   cJSON_AddStringToObject(payload, "provider", provider ? provider : "");
   cJSON_AddStringToObject(payload, "model", model ? model : "");
   cJSON_AddNumberToObject(payload, "http_status", http_status);
   cJSON_AddStringToObject(payload, "reason", failover_reason_name(reason));
   cJSON_AddStringToObject(payload, "action", failover_action_name(action));
   cJSON_AddNumberToObject(payload, "attempt", attempt);
   cJSON_AddBoolToObject(payload, "recovered", recovered ? 1 : 0);

   char *payload_json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (!payload_json)
      return -1;

   int rc = db1_interaction_event_record(session_id, ie_event_type_name(IE_FAILOVER_EVENT),
                                         "system", payload_json, recovered ? "ok" : "error");
   free(payload_json);
   return rc;
}
