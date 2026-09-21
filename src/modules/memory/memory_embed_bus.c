/* Embedding transport for the Go memory process.
 *
 * This is deliberately a C connection adapter, not an embedding
 * implementation. It validates and translates the JSON reply from the local
 * event bus; model access, breaker state, and vector production stay in Go.
 */
#include "aimee.h"
#include "headers/module_json_call.h"

#include <aimee/memory/module_api.h>
#include <aimee/core/event_bus/module_protocol.h>

#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY_EMBED_BUS_TIMEOUT_MS 25000

static _Thread_local int memory_last_unauthorized;

int memory_embed_command_is_http(const char *command)
{
   return command && (strncmp(command, "http://", 7) == 0 || strncmp(command, "https://", 8) == 0);
}

static int optional_bool(const cJSON *root, const char *name, int *value)
{
   const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
   if (!item)
   {
      *value = 0;
      return 0;
   }
   if (!cJSON_IsBool(item))
      return -1;
   *value = cJSON_IsTrue(item) ? 1 : 0;
   return 0;
}

int memory_embed_text(const char *text, const char *command, embed_input_type_t input_type,
                      float *out, int max_dim)
{
   memory_last_unauthorized = 0;
   if (!text || !text[0] || !command || !command[0] || !out || max_dim <= 0 ||
       (input_type != EMBED_INPUT_DOCUMENT && input_type != EMBED_INPUT_QUERY))
      return 0;

   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "base_url", command) ||
       !cJSON_AddStringToObject(request, "input_type",
                               input_type == EMBED_INPUT_QUERY ? "query" : "document") ||
       !cJSON_AddStringToObject(request, "text", text) ||
       !cJSON_AddNumberToObject(request, "max_dim", max_dim))
   {
      cJSON_Delete(request);
      return 0;
   }

   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response = aimee_module_json_call(AIMEE_MEMORY_EVENT_EMBED, AIMEE_MEMORY_STAGE_EMBED,
                                            request, AIMEE_MODULE_MESSAGE_MAX_BODY,
                                            MEMORY_EMBED_BUS_TIMEOUT_MS, &result);
   if (!response)
      return 0;

   const cJSON *json_dim = cJSON_GetObjectItemCaseSensitive(response, "dim");
   const cJSON *vector = cJSON_GetObjectItemCaseSensitive(response, "vector");
   const cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
   int unavailable = 0, unauthorized = 0, truncated = 0;
   int dim = 0;
   if (optional_bool(response, "unauthorized", &unauthorized) != 0)
      goto done;
   memory_last_unauthorized = unauthorized;
   if (unauthorized)
      goto done;
   if (!cJSON_IsNumber(json_dim) || json_dim->valuedouble != (double)json_dim->valueint ||
       json_dim->valueint <= 0 || json_dim->valueint > max_dim || !cJSON_IsArray(vector) ||
       cJSON_GetArraySize(vector) != json_dim->valueint ||
       (error && (!cJSON_IsString(error) || (error->valuestring && error->valuestring[0]))) ||
       optional_bool(response, "unavailable", &unavailable) != 0 || unavailable ||
       optional_bool(response, "truncated", &truncated) != 0 || truncated)
      goto done;

   for (int i = 0; i < json_dim->valueint; ++i)
   {
      const cJSON *component = cJSON_GetArrayItem(vector, i);
      if (!cJSON_IsNumber(component) || !isfinite(component->valuedouble))
         goto done;
      out[i] = (float)component->valuedouble;
      if (!isfinite(out[i]))
         goto done;
   }
   dim = json_dim->valueint;

done:
   cJSON_Delete(response);
   return dim;
}

int memory_embedder_last_result_unauthorized(void)
{
   return memory_last_unauthorized;
}

int memory_embed_serving_id(const char *command, char *out, size_t out_len)
{
   if (!command || !command[0] || !out || out_len == 0) return -1;
   out[0] = '\0';
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "serving-id") ||
       !cJSON_AddStringToObject(request, "base_url", command))
   {
      cJSON_Delete(request);
      return -1;
   }
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response = aimee_module_json_call(AIMEE_MEMORY_EVENT_EMBED, AIMEE_MEMORY_STAGE_EMBED,
                                            request, AIMEE_MODULE_MESSAGE_MAX_BODY,
                                            MEMORY_EMBED_BUS_TIMEOUT_MS, &result);
   const cJSON *serving = response ? cJSON_GetObjectItemCaseSensitive(response, "serving_id") : NULL;
   const cJSON *error = response ? cJSON_GetObjectItemCaseSensitive(response, "error") : NULL;
   if (!cJSON_IsString(serving) || !serving->valuestring ||
       (cJSON_IsString(error) && error->valuestring && error->valuestring[0]))
   {
      cJSON_Delete(response);
      return -1;
   }
   snprintf(out, out_len, "%s", serving->valuestring);
   cJSON_Delete(response);
   return 0;
}

int memory_embed_texts(const char *const *texts, int n, const char *command,
                       embed_input_type_t input_type, float *out, int dim)
{
   if (!texts || n <= 0 || !out || dim <= 0)
      return 0;
   for (int i = 0; i < n; ++i)
      if (memory_embed_text(texts[i], command, input_type, out + (size_t)i * (size_t)dim, dim) != dim)
         return 0;
   return n;
}
