/* Host-side transport for native benchmark callers of the Go memory owner.
 * This belongs to the benchmark host, not the memory process. The memory-side
 * producer/consumer and all persistence, ranking, lifecycle and scope decisions
 * are Go. The existing C bus remains the transport substrate.
 */
#include "aimee.h"
#include "headers/module_json_call.h"

#include "headers/memory_stage_contract.h"
#include <aimee/core/event_bus/module_protocol.h>

#include "cJSON.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MEMORY_DATA_TIMEOUT_MS 5000

static cJSON *memory_data_call(cJSON *request)
{
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   return aimee_module_json_call(AIMEE_MEMORY_EVENT_DATA, AIMEE_MEMORY_STAGE_DATA, request,
                                 AIMEE_MODULE_MESSAGE_MAX_BODY, MEMORY_DATA_TIMEOUT_MS, &result);
}

static int copy_string(char *out, size_t cap, const cJSON *object, const char *name)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
   if (!cJSON_IsString(value) || !value->valuestring || strlen(value->valuestring) >= cap)
      return -1;
   memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
   return 0;
}

static int memory_from_json(const cJSON *object, memory_t *out)
{
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
   const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(object, "confidence");
   if (!out || !cJSON_IsObject(object) || !cJSON_IsNumber(id) || id->valuedouble <= 0 ||
       !cJSON_IsNumber(confidence))
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = (int64_t)id->valuedouble;
   out->confidence = confidence->valuedouble;
   const cJSON *content = cJSON_GetObjectItemCaseSensitive(object, "content");
   if (!cJSON_IsString(content))
      return -1;
   size_t length = strlen(content->valuestring);
   if (length >= sizeof(out->content))
   {
      length = sizeof(out->content) - 1;
      while (length && ((unsigned char)content->valuestring[length] & 0xc0) == 0x80)
         --length;
   }
   memcpy(out->content, content->valuestring, length);
   out->content[length] = '\0';
   return copy_string(out->tier, sizeof(out->tier), object, "tier") == 0 &&
                  copy_string(out->kind, sizeof(out->kind), object, "kind") == 0 &&
                  copy_string(out->key, sizeof(out->key), object, "key") == 0
              ? 0
              : -1;
}

static int records_from_response(cJSON *response, memory_t *out, int max)
{
   const cJSON *records = response ? cJSON_GetObjectItemCaseSensitive(response, "records") : NULL;
   if (!cJSON_IsArray(records) || !out || max <= 0)
      return -1;
   int count = cJSON_GetArraySize(records);
   if (count > max)
      count = max;
   for (int i = 0; i < count; ++i)
      if (memory_from_json(cJSON_GetArrayItem(records, i), &out[i]) != 0)
         return -1;
   return count;
}

int memory_insert_epistemic_ex(const char *tier, const char *kind, const char *epistemic_kind,
                               const char *key, const char *content, const char *use_cases,
                               double confidence, const char *session_id,
                               memory_authority_t authority, memory_t *out)
{
   if (!tier || !tier[0] || !kind || !kind[0] || !key || !key[0] || !content || !content[0] ||
       confidence < 0.0 || confidence > 1.0 ||
       (authority != MEMORY_AUTHORITY_MODEL && authority != MEMORY_AUTHORITY_USER))
      return -1;
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "insert-epistemic") ||
       !cJSON_AddStringToObject(request, "tier", tier) ||
       !cJSON_AddStringToObject(request, "kind", kind) ||
       !cJSON_AddStringToObject(request, "epistemic_kind",
                                epistemic_kind && epistemic_kind[0] ? epistemic_kind
                                                                    : "world_fact") ||
       !cJSON_AddStringToObject(request, "key", key) ||
       !cJSON_AddStringToObject(request, "content", content) ||
       !cJSON_AddStringToObject(request, "use_cases", use_cases ? use_cases : "") ||
       !cJSON_AddNumberToObject(request, "confidence", confidence) ||
       !cJSON_AddNumberToObject(request, "authority", authority) ||
       (session_id && session_id[0] && !cJSON_AddStringToObject(request, "session_id", session_id)))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = memory_data_call(request);
   memory_t ignored;
   int count = records_from_response(response, out ? out : &ignored, 1);
   cJSON_Delete(response);
   return count == 1 ? 0 : -1;
}

int memory_insert_ex(const char *tier, const char *kind, const char *key, const char *content,
                     const char *use_cases, double confidence, const char *session_id,
                     memory_authority_t authority, memory_t *out)
{
   return memory_insert_epistemic_ex(tier, kind, "world_fact", key, content, use_cases, confidence,
                                     session_id, authority, out);
}

int memory_insert(const char *tier, const char *kind, const char *key, const char *content,
                  double confidence, const char *session_id, memory_t *out)
{
   return memory_insert_ex(tier, kind, key, content, "", confidence, session_id,
                           MEMORY_AUTHORITY_MODEL, out);
}
