/* Legacy typed-fact recall ABI over the Go memory module.
 *
 * This C file is connection code only: it builds the bounded stage-7 request,
 * invokes the event bus, validates the response, and copies the finished block
 * into the caller's buffer. SQL, entity selection, ranking, formatting, and
 * PII policy live in server-go/modules/memory/fact_recall.go.
 */
#include "fact_recall.h"
#include "headers/module_json_call.h"

#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/memory/module_api.h>

#include "cJSON.h"

#include <string.h>

#define FACT_RECALL_TIMEOUT_MS 5000
#define FACT_RECALL_MAX_CAP (512u * 1024u)

static int fact_recall_call(const char *field, const char *value, int turn_requests_sensitive,
                            char *out, size_t cap)
{
   if (!field || !value || !out || cap == 0 || cap > FACT_RECALL_MAX_CAP)
      return -1;
   out[0] = '\0';

   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "fact-recall") ||
       !cJSON_AddStringToObject(request, field, value) ||
       !cJSON_AddBoolToObject(request, "turn_requests_sensitive", turn_requests_sensitive != 0) ||
       !cJSON_AddNumberToObject(request, "content_capacity", (double)cap))
   {
      cJSON_Delete(request);
      return -1;
   }

   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response = aimee_module_json_call(
       AIMEE_MEMORY_EVENT_DATA, AIMEE_MEMORY_STAGE_DATA, request, AIMEE_MODULE_MESSAGE_MAX_BODY,
       FACT_RECALL_TIMEOUT_MS, &result);
   const cJSON *block = response ? cJSON_GetObjectItemCaseSensitive(response, "block") : NULL;
   const cJSON *count = response ? cJSON_GetObjectItemCaseSensitive(response, "count") : NULL;
   if ((!block || (!cJSON_IsString(block) && !cJSON_IsNull(block))) || !cJSON_IsNumber(count) ||
       count->valueint < 0)
   {
      cJSON_Delete(response);
      return -1;
   }

   const char *text = cJSON_IsString(block) && block->valuestring ? block->valuestring : "";
   size_t len = strlen(text);
   if (len >= cap)
   {
      cJSON_Delete(response);
      return -1;
   }
   memcpy(out, text, len + 1);
   int written = count->valueint;
   cJSON_Delete(response);
   return written;
}

int db2_fact_recall_block(const char *entity, int turn_requests_sensitive, char *out, size_t cap)
{
   if (!entity || !entity[0])
      return -1;
   return fact_recall_call("entity", entity, turn_requests_sensitive, out, cap);
}

int db2_fact_recall_in_query(const char *query, int turn_requests_sensitive, char *out, size_t cap)
{
   if (!query)
      return -1;
   return fact_recall_call("query", query, turn_requests_sensitive, out, cap);
}
