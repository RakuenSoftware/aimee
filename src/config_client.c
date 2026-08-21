/* Native caller contract for the independently running pure-Go config module.
 * This file contains no config parser, schema, defaults, or storage. It only
 * uses the existing generic JSON-over-event-bus call path. */
#include "config_client.h"
#include "module_json_call.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_CLIENT_MAX_BODY   (16u * 1024u * 1024u)
#define CONFIG_CLIENT_TIMEOUT_MS 5000

static pthread_mutex_t g_config_client_lock = PTHREAD_MUTEX_INITIALIZER;
static cJSON *g_config_client_values;
static char g_config_client_version[65];
static _Thread_local char g_config_client_error[160];

/* Narrow transport seam used by contract tests. Production resolves this weak
 * default, which is the existing generic JSON event-bus caller. */
__attribute__((weak)) cJSON *config_client_transport_call(uint32_t event_kind, uint32_t stage_id,
                                                          cJSON *request, size_t max_body,
                                                          int timeout_ms,
                                                          aimee_module_call_result_t *result)
{
   return aimee_module_json_call(event_kind, stage_id, request, max_body, timeout_ms, result);
}

static const char *config_client_result_name(aimee_module_call_result_t result)
{
   switch (result)
   {
   case AIMEE_MODULE_CALL_CAPABILITY_ABSENT:
      return "config module unavailable";
   case AIMEE_MODULE_CALL_DEADLINE_EXCEEDED:
      return "config module timeout";
   case AIMEE_MODULE_CALL_CANCELLED:
      return "config module call cancelled";
   case AIMEE_MODULE_CALL_INVALID_REQUEST:
      return "config module rejected request";
   case AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE:
      return "config module response too large";
   default:
      return "config module transport failure";
   }
}

static void config_client_error(const char *message)
{
   snprintf(g_config_client_error, sizeof(g_config_client_error), "%s", message ? message : "");
}

const char *config_client_last_error(void)
{
   return g_config_client_error;
}

int config_client_key_is_secret(const char *key)
{
   return config_client_secret_name(key) != NULL;
}

const char *config_client_secret_name(const char *key)
{
   static const struct
   {
      const char *key;
      const char *name;
   } secrets[] = {{"db2_url", "AIMEE_DB2_URL"},
                  {"search_tavily_api_key", "AIMEE_SEARCH_TAVILY_API_KEY"},
                  {"proxy_token", "AIMEE_PROXY_TOKEN"},
                  {"ingress_trusted_proxy_secret", "AIMEE_INGRESS_PROXY_SECRET"},
                  {"kb_api_bearer_token", "AIMEE_KB_API_BEARER_TOKEN"},
                  {"telemetry_metrics_token", "AIMEE_TELEMETRY_METRICS_TOKEN"},
                  {"kb_client_bearer_token", "AIMEE_KB_CLIENT_BEARER_TOKEN"},
                  {"server_api_bearer_token", "AIMEE_API_BEARER_TOKEN"},
                  {"trigger_auth_token", "AIMEE_TRIGGER_AUTH_TOKEN"},
                  {"kb_curator_provider_api_key", "AIMEE_KB_CURATOR_PROVIDER_API_KEY"},
                  {"embedder_api_key", "EMBEDDER_API_KEY"},
                  {"synthesis_api_key", "SYNTHESIS_API_KEY"}};
   if (!key)
      return NULL;
   for (size_t i = 0; i < sizeof(secrets) / sizeof(secrets[0]); i++)
      if (!strcmp(key, secrets[i].key))
         return secrets[i].name;
   return NULL;
}

static int config_client_fetch_locked(void)
{
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "snapshot"))
   {
      cJSON_Delete(request);
      config_client_error("cannot allocate config request");
      return -1;
   }
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response =
       config_client_transport_call(AIMEE_CONFIG_EVENT_KIND, AIMEE_CONFIG_STAGE_ID, request,
                                    CONFIG_CLIENT_MAX_BODY, CONFIG_CLIENT_TIMEOUT_MS, &result);
   if (!response)
   {
      config_client_error(config_client_result_name(result));
      return -1;
   }
   cJSON *ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
   cJSON *values = cJSON_GetObjectItemCaseSensitive(response, "values");
   cJSON *version = cJSON_GetObjectItemCaseSensitive(response, "version");
   if (!cJSON_IsTrue(ok) || !cJSON_IsObject(values))
   {
      cJSON *message = cJSON_GetObjectItemCaseSensitive(response, "error");
      config_client_error(cJSON_IsString(message) ? message->valuestring
                                                  : "malformed config reply");
      cJSON_Delete(response);
      return -1;
   }
   cJSON *copy = cJSON_Duplicate(values, 1);
   char version_copy[65] = {0};
   if (cJSON_IsString(version) && strlen(version->valuestring) == 64)
      snprintf(version_copy, sizeof(version_copy), "%s", version->valuestring);
   cJSON_Delete(response);
   if (!copy)
   {
      config_client_error("cannot retain config reply");
      return -1;
   }
   cJSON_Delete(g_config_client_values);
   g_config_client_values = copy;
   snprintf(g_config_client_version, sizeof(g_config_client_version), "%s", version_copy);
   config_client_error("");
   return 0;
}

int config_client_changed(void)
{
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "version"))
   {
      cJSON_Delete(request);
      return -1;
   }
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response =
       config_client_transport_call(AIMEE_CONFIG_EVENT_KIND, AIMEE_CONFIG_STAGE_ID, request,
                                    CONFIG_CLIENT_MAX_BODY, CONFIG_CLIENT_TIMEOUT_MS, &result);
   if (!response)
   {
      config_client_error(config_client_result_name(result));
      return -1;
   }
   cJSON *ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
   cJSON *version = cJSON_GetObjectItemCaseSensitive(response, "version");
   if (!cJSON_IsTrue(ok) || !cJSON_IsString(version) || strlen(version->valuestring) != 64)
   {
      cJSON_Delete(response);
      config_client_error("malformed config version reply");
      return -1;
   }
   pthread_mutex_lock(&g_config_client_lock);
   int changed = !g_config_client_values || !g_config_client_version[0] ||
                 strcmp(g_config_client_version, version->valuestring) != 0;
   pthread_mutex_unlock(&g_config_client_lock);
   cJSON_Delete(response);
   config_client_error("");
   return changed;
}

int config_client_refresh(void)
{
   pthread_mutex_lock(&g_config_client_lock);
   int rc = config_client_fetch_locked();
   pthread_mutex_unlock(&g_config_client_lock);
   return rc;
}

static int config_client_ensure_locked(void)
{
   const char *no_cache = getenv("AIMEE_NO_CACHE");
   return g_config_client_values && !(no_cache && no_cache[0] && strcmp(no_cache, "0") != 0)
              ? 0
              : config_client_fetch_locked();
}

int config_client_read_number(const char *key, double *out)
{
   if (!key || !out)
      return -1;
   pthread_mutex_lock(&g_config_client_lock);
   int rc = config_client_ensure_locked();
   cJSON *value = rc == 0 ? cJSON_GetObjectItemCaseSensitive(g_config_client_values, key) : NULL;
   if (cJSON_IsBool(value))
      *out = cJSON_IsTrue(value) ? 1.0 : 0.0;
   else if (cJSON_IsNumber(value))
      *out = value->valuedouble;
   else
      rc = -1;
   pthread_mutex_unlock(&g_config_client_lock);
   return rc;
}

int config_client_read_string(const char *key, char *out, size_t n)
{
   if (!key || !out || n == 0)
      return -1;
   out[0] = 0;
   pthread_mutex_lock(&g_config_client_lock);
   int rc = config_client_ensure_locked();
   cJSON *value = rc == 0 ? cJSON_GetObjectItemCaseSensitive(g_config_client_values, key) : NULL;
   if (cJSON_IsString(value))
      snprintf(out, n, "%s", value->valuestring);
   else
      rc = -1;
   pthread_mutex_unlock(&g_config_client_lock);
   return rc;
}

static cJSON *config_client_indexed_locked(const char *key, int index, const char *member)
{
   cJSON *array = cJSON_GetObjectItemCaseSensitive(g_config_client_values, key);
   cJSON *value = cJSON_IsArray(array) && index >= 0 ? cJSON_GetArrayItem(array, index) : NULL;
   if (member && cJSON_IsObject(value))
      value = cJSON_GetObjectItemCaseSensitive(value, member);
   return value;
}

int config_client_read_indexed_number(const char *key, int index, const char *member, double *out)
{
   if (!key || !out)
      return -1;
   pthread_mutex_lock(&g_config_client_lock);
   int rc = config_client_ensure_locked();
   cJSON *value = rc == 0 ? config_client_indexed_locked(key, index, member) : NULL;
   if (cJSON_IsBool(value))
      *out = cJSON_IsTrue(value) ? 1.0 : 0.0;
   else if (cJSON_IsNumber(value))
      *out = value->valuedouble;
   else
      rc = -1;
   pthread_mutex_unlock(&g_config_client_lock);
   return rc;
}

int config_client_read_indexed_string(const char *key, int index, const char *member, char *out,
                                      size_t n)
{
   if (!key || !out || n == 0)
      return -1;
   out[0] = 0;
   pthread_mutex_lock(&g_config_client_lock);
   int rc = config_client_ensure_locked();
   cJSON *value = rc == 0 ? config_client_indexed_locked(key, index, member) : NULL;
   if (cJSON_IsString(value))
      snprintf(out, n, "%s", value->valuestring);
   else
      rc = -1;
   pthread_mutex_unlock(&g_config_client_lock);
   return rc;
}

cJSON *config_client_value_copy(const char *key)
{
   if (!key)
      return NULL;
   pthread_mutex_lock(&g_config_client_lock);
   cJSON *copy = NULL;
   if (config_client_ensure_locked() == 0)
   {
      cJSON *value = cJSON_GetObjectItemCaseSensitive(g_config_client_values, key);
      if (value)
         copy = cJSON_Duplicate(value, 1);
   }
   pthread_mutex_unlock(&g_config_client_lock);
   return copy;
}

cJSON *config_client_snapshot_copy(void)
{
   pthread_mutex_lock(&g_config_client_lock);
   cJSON *copy =
       config_client_ensure_locked() == 0 ? cJSON_Duplicate(g_config_client_values, 1) : NULL;
   pthread_mutex_unlock(&g_config_client_lock);
   return copy;
}

int config_client_set_value(const char *key, cJSON *value)
{
   if (!key || !value)
   {
      cJSON_Delete(value);
      return -1;
   }
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "set-versioned") ||
       !cJSON_AddStringToObject(request, "key", key))
   {
      cJSON_Delete(request);
      cJSON_Delete(value);
      return -1;
   }
   cJSON_AddItemToObject(request, "value", value);
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response =
       config_client_transport_call(AIMEE_CONFIG_EVENT_KIND, AIMEE_CONFIG_STAGE_ID, request,
                                    CONFIG_CLIENT_MAX_BODY, CONFIG_CLIENT_TIMEOUT_MS, &result);
   if (!response)
   {
      config_client_error(config_client_result_name(result));
      return -1;
   }
   int rc = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ok")) ? 0 : -1;
   if (rc != 0)
   {
      cJSON *message = cJSON_GetObjectItemCaseSensitive(response, "error");
      config_client_error(cJSON_IsString(message) ? message->valuestring
                                                  : "config mutation failed");
   }
   cJSON_Delete(response);
   if (rc == 0)
      rc = config_client_refresh();
   return rc;
}

int config_client_set_number(const char *key, double value)
{
   return config_client_set_value(key, cJSON_CreateNumber(value));
}

int config_client_set_string(const char *key, const char *value)
{
   return config_client_set_value(key, cJSON_CreateString(value ? value : ""));
}

int config_client_operation(const char *operation, cJSON *value)
{
   if (!operation || !operation[0])
   {
      cJSON_Delete(value);
      return -1;
   }
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", operation))
   {
      cJSON_Delete(request);
      cJSON_Delete(value);
      return -1;
   }
   if (value)
      cJSON_AddItemToObject(request, "value", value);
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response =
       config_client_transport_call(AIMEE_CONFIG_EVENT_KIND, AIMEE_CONFIG_STAGE_ID, request,
                                    CONFIG_CLIENT_MAX_BODY, CONFIG_CLIENT_TIMEOUT_MS, &result);
   if (!response)
   {
      config_client_error(config_client_result_name(result));
      return -1;
   }
   int rc = 0;
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ok")))
   {
      cJSON *code = cJSON_GetObjectItemCaseSensitive(response, "code");
      cJSON *message = cJSON_GetObjectItemCaseSensitive(response, "error");
      config_client_error(cJSON_IsString(message) ? message->valuestring
                                                  : "config mutation failed");
      if (cJSON_IsString(code) &&
          (!strcmp(code->valuestring, "exists") || !strcmp(code->valuestring, "not_found")))
         rc = -2;
      else if (cJSON_IsString(code) && !strcmp(code->valuestring, "full"))
         rc = -3;
      else
         rc = -1;
   }
   cJSON_Delete(response);
   if (rc == 0 || rc == -2)
      if (config_client_refresh() != 0)
         return -1;
   return rc;
}
