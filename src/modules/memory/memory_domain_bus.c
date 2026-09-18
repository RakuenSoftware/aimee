/* Legacy C ABI adapter for the Go-owned memory domain.
 *
 * This translation unit is deliberately transport-only: it bounds and encodes
 * requests for stage 7 and copies typed replies into the structs still used by
 * the CLI/KB edge. Persistence, lifecycle, matching, ranking and scope policy
 * all live in server-go/modules/memory.
 */
#include "aimee.h"
#include "headers/module_json_call.h"

#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/memory/module_api.h>

#include "cJSON.h"
#include "memory_query.h"
#include "memory_scope_query.h"
#include "memory_bus_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOMAIN_TIMEOUT_MS 5000

static cJSON *domain_call(cJSON *request)
{
   if (memory_bus_add_context(request) != 0)
   {
      cJSON_Delete(request);
      return NULL;
   }
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   return aimee_module_json_call(AIMEE_MEMORY_EVENT_DATA, AIMEE_MEMORY_STAGE_DATA, request,
                                 AIMEE_MODULE_MESSAGE_MAX_BODY, DOMAIN_TIMEOUT_MS, &result);
}

static cJSON *domain_request(const char *operation)
{
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", operation))
   {
      cJSON_Delete(request);
      return NULL;
   }
   return request;
}

int db2_memory_key_exists(const char *key)
{
   if (!key || !key[0])
      return 0;
   cJSON *request = domain_request("key-exists");
   if (!request || !cJSON_AddStringToObject(request, "key", key))
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON *response = domain_call(request);
   const cJSON *allowed = response ? cJSON_GetObjectItemCaseSensitive(response, "allowed") : NULL;
   int result = cJSON_IsBool(allowed) ? cJSON_IsTrue(allowed) : -1;
   cJSON_Delete(response);
   return result;
}
