/* Owned response snapshots for internal result caches, never native ingress or
 * durable session envelopes. Sidecars remain opaque and cannot authorize work. */
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const usage_keys[] = {"input", "output", "cache_read", "cache_write",
                                         "reasoning"};
static int add_copy(cJSON *object, const char *key, const cJSON *source)
{
   if (!source)
      return 0;
   cJSON *copy = cJSON_Duplicate(source, 1);
   if (copy && cJSON_AddItemToObject(object, key, copy))
      return 0;
   cJSON_Delete(copy);
   return -1;
}
static int optional_string(cJSON *object, const char *key, const char *value)
{
   return value && !cJSON_AddStringToObject(object, key, value) ? -1 : 0;
}

cJSON *aimee_ir_response_json(const aimee_response_t *response)
{
   if (!response || (response->role && strcmp(response->role, "assistant")) ||
       response->stop_reason < AIMEE_STOP_UNKNOWN || response->stop_reason > AIMEE_STOP_ERROR)
      return NULL;
   aimee_response_t copy = {0};
   if (aimee_ir_response_clone(response, &copy))
      return NULL;
   aimee_message_t message = {.role = copy.role ? copy.role : "assistant",
                              .blocks = copy.content,
                              .n_blocks = copy.n_content};
   cJSON *json = aimee_ir_message_json(&message);
   cJSON *usage = json ? cJSON_AddObjectToObject(json, "usage") : NULL;
   if (json && !copy.role)
   {
      cJSON *implicit = cJSON_CreateNull();
      if (!implicit || !cJSON_ReplaceItemInObjectCaseSensitive(json, "role", implicit))
      {
         cJSON_Delete(implicit);
         goto fail;
      }
   }
   if (!json || !usage || !cJSON_AddStringToObject(json, "schema", "aimee.response.v1") ||
       !cJSON_AddNumberToObject(json, "stop_reason", copy.stop_reason) ||
       optional_string(json, "id", copy.id) || optional_string(json, "model", copy.model) ||
       optional_string(json, "error_message", copy.error_message) ||
       optional_string(json, "raw_stop_reason", copy.raw_stop_reason) ||
       add_copy(json, "raw", copy.raw))
      goto fail;
   long values[] = {copy.usage_in, copy.usage_out, copy.usage_cache_read, copy.usage_cache_write,
                    copy.usage_reasoning};
   for (size_t i = 0; i < sizeof values / sizeof *values; i++)
   {
      char decimal[32];
      snprintf(decimal, sizeof decimal, "%ld", values[i]);
      if (!cJSON_AddStringToObject(usage, usage_keys[i], decimal))
         goto fail;
   }
   cJSON *blocks = cJSON_GetObjectItemCaseSensitive(json, "blocks");
   for (int i = 0; i < copy.n_content; i++)
      if (add_copy(cJSON_GetArrayItem(blocks, i), "raw", copy.content[i].raw))
         goto fail;
   aimee_response_free(&copy);
   return json;
fail:
   aimee_response_free(&copy);
   cJSON_Delete(json);
   return NULL;
}
static int read_optional(const cJSON *json, const char *key, char **out)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
   if (!value)
      return 0;
   if (!cJSON_IsString(value))
      return -1;
   *out = strdup(value->valuestring);
   return *out ? 0 : -1;
}
static int read_usage(const cJSON *usage, const char *key, long *out)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(usage, key);
   if (!cJSON_IsString(value) || !value->valuestring[0])
      return -1;
   const char *text = value->valuestring;
   if ((text[0] == '0' && text[1]) || text[0] < '0' || text[0] > '9')
      return -1;
   char *end = NULL;
   errno = 0;
   long parsed = strtol(text, &end, 10);
   if (errno || !end || *end || parsed < 0)
      return -1;
   *out = parsed;
   return 0;
}
int aimee_ir_response_parse(const cJSON *json, aimee_response_t *out)
{
   if (!out || !cJSON_IsObject(json) || !aimee_ir_json_unique(json))
      return -1;
   static const char *const fields[] = {"schema",          "role", "blocks", "usage",
                                        "stop_reason",     "id",   "model",  "error_message",
                                        "raw_stop_reason", "raw"};
   const cJSON *field;
   cJSON_ArrayForEach(field, json)
   {
      int known = 0;
      for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
         if (!strcmp(field->string, fields[i]))
            known = 1;
      if (!known)
         return -1;
   }
   const cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema");
   const cJSON *stop = cJSON_GetObjectItemCaseSensitive(json, "stop_reason");
   const cJSON *usage = cJSON_GetObjectItemCaseSensitive(json, "usage");
   if (!cJSON_IsString(schema) || strcmp(schema->valuestring, "aimee.response.v1") ||
       !cJSON_IsNumber(stop) || stop->valuedouble != stop->valueint ||
       stop->valueint < AIMEE_STOP_UNKNOWN || stop->valueint > AIMEE_STOP_ERROR ||
       !cJSON_IsObject(usage) || cJSON_GetArraySize(usage) != 5)
      return -1;
   aimee_message_t message = {0};
   aimee_response_t result = {0};
   const cJSON *role = cJSON_GetObjectItemCaseSensitive(json, "role");
   if (!cJSON_IsNull(role) && (!cJSON_IsString(role) || strcmp(role->valuestring, "assistant")))
      return -1;
   cJSON *view = NULL;
   if (cJSON_IsNull(role))
   {
      view = cJSON_Duplicate(json, 1);
      cJSON *explicit_role = cJSON_CreateString("assistant");
      if (!view || !explicit_role ||
          !cJSON_ReplaceItemInObjectCaseSensitive(view, "role", explicit_role))
      {
         cJSON_Delete(view);
         cJSON_Delete(explicit_role);
         return -1;
      }
   }
   int parsed = aimee_ir_message_parse(view ? view : json, &message);
   cJSON_Delete(view);
   if (parsed)
      return -1;
   if (cJSON_IsNull(role))
   {
      free(message.role);
      message.role = NULL;
   }
   result.role = message.role;
   result.content = message.blocks;
   result.n_content = message.n_blocks;
   result.stop_reason = (aimee_stop_reason_t)stop->valueint;
   long *values[] = {&result.usage_in, &result.usage_out, &result.usage_cache_read,
                     &result.usage_cache_write, &result.usage_reasoning};
   for (size_t i = 0; i < sizeof values / sizeof *values; i++)
      if (read_usage(usage, usage_keys[i], values[i]))
         goto fail;
   if (read_optional(json, "id", &result.id) || read_optional(json, "model", &result.model) ||
       read_optional(json, "error_message", &result.error_message) ||
       read_optional(json, "raw_stop_reason", &result.raw_stop_reason))
      goto fail;
   const cJSON *raw = cJSON_GetObjectItemCaseSensitive(json, "raw");
   if (raw && !(result.raw = cJSON_Duplicate(raw, 1)))
      goto fail;
   const cJSON *blocks = cJSON_GetObjectItemCaseSensitive(json, "blocks");
   for (int i = 0; i < result.n_content; i++)
   {
      raw = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(blocks, i), "raw");
      if (raw && !(result.content[i].raw = cJSON_Duplicate(raw, 1)))
         goto fail;
   }
   /* Clone performs read-only admission before replacing any caller storage. */
   if (aimee_ir_response_clone(&result, out))
      goto fail;
   aimee_response_free(&result);
   return 0;
fail:
   aimee_response_free(&result);
   return -1;
}
