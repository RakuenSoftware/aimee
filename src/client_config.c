#include "client_config.h"

#include "cli_client.h"

#include <stdio.h>
#include <string.h>

static cJSON *(*g_provider)(const char *key);
static cJSON *(*g_operation_provider)(const char *operation, const cJSON *value);

/* Core is shared with daemon/test binaries that do not link the thin-client
 * dispatcher. A weak declaration keeps the dependency one-way while the real
 * CLI resolves it normally. */
#if defined(__GNUC__)
extern cJSON *cli_v1_dispatch(cJSON *req, int timeout_ms) __attribute__((weak));
#endif

void client_config_set_provider(cJSON *(*provider)(const char *key))
{
   g_provider = provider;
}

void client_config_set_operation_provider(cJSON *(*provider)(const char *operation,
                                                             const cJSON *value))
{
   g_operation_provider = provider;
}

static cJSON *client_config_operation_response(const char *operation, cJSON *value)
{
   if (!operation || !operation[0])
   {
      cJSON_Delete(value);
      return NULL;
   }
   if (g_operation_provider)
   {
      cJSON *response = g_operation_provider(operation, value);
      cJSON_Delete(value);
      return response;
   }
#if defined(__GNUC__)
   if (!cli_v1_dispatch)
   {
      cJSON_Delete(value);
      return NULL;
   }
#endif
   cJSON *request = cJSON_CreateObject();
   if (!request)
   {
      cJSON_Delete(value);
      return NULL;
   }
   cJSON_AddStringToObject(request, "method", "config.set");
   cJSON_AddStringToObject(request, "operation", operation);
   if (value)
      cJSON_AddItemToObject(request, "value", value);
   cJSON *response = cli_v1_dispatch(request, 5000);
   cJSON_Delete(request);
   return response;
}

int client_config_operation(const char *operation, cJSON *value)
{
   cJSON *response = client_config_operation_response(operation, value);
   cJSON *status = cJSON_GetObjectItemCaseSensitive(response, "status");
   int rc = cJSON_IsObject(response) &&
                    (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ok")) ||
                     (cJSON_IsString(status) && !strcmp(status->valuestring, "ok")))
                ? 0
                : -1;
   cJSON_Delete(response);
   return rc;
}

int client_config_profile_present(const char *name)
{
   cJSON *value = cJSON_CreateObject();
   if (!value || !cJSON_AddStringToObject(value, "name", name ? name : ""))
   {
      cJSON_Delete(value);
      return -1;
   }
   cJSON *response = client_config_operation_response("profile-present", value);
   cJSON *present = cJSON_GetObjectItemCaseSensitive(response, "present");
   int rc = cJSON_IsBool(present) ? cJSON_IsTrue(present) : -1;
   cJSON_Delete(response);
   return rc;
}

cJSON *client_config_value(const char *key)
{
   if (!key || !key[0])
      return NULL;
   if (g_provider)
      return g_provider(key);
#if defined(__GNUC__)
   if (!cli_v1_dispatch)
      return NULL;
#endif
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return NULL;
   cJSON_AddStringToObject(request, "method", "config.get");
   cJSON_AddStringToObject(request, "key", key);
   cJSON *response = cli_v1_dispatch(request, 5000);
   cJSON_Delete(request);
   if (!cJSON_IsObject(response))
   {
      cJSON_Delete(response);
      return NULL;
   }
   cJSON *value = cJSON_GetObjectItemCaseSensitive(response, "value");
   if (!value)
   {
      cJSON *config = cJSON_GetObjectItemCaseSensitive(response, "config");
      value = cJSON_IsObject(config) ? cJSON_GetObjectItemCaseSensitive(config, key) : NULL;
   }
   cJSON *copy = value ? cJSON_Duplicate(value, 1) : NULL;
   cJSON_Delete(response);
   return copy;
}

int client_config_bool(const char *key, int fallback)
{
   cJSON *value = client_config_value(key);
   int result = cJSON_IsBool(value) ? cJSON_IsTrue(value) : fallback;
   cJSON_Delete(value);
   return result;
}

int client_config_int(const char *key, int fallback)
{
   cJSON *value = client_config_value(key);
   int result = cJSON_IsNumber(value) ? value->valueint : fallback;
   cJSON_Delete(value);
   return result;
}

int client_config_string(const char *key, char *out, unsigned long out_size, const char *fallback)
{
   if (!out || out_size == 0)
      return -1;
   cJSON *value = client_config_value(key);
   const char *text = cJSON_IsString(value) ? value->valuestring : fallback;
   snprintf(out, (size_t)out_size, "%s", text ? text : "");
   int found = cJSON_IsString(value);
   cJSON_Delete(value);
   return found;
}
