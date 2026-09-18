#include "module_commands.h"
#include "json_fluent.h"
/* Transitional transports for Go-owned semantic search and typed context. */

#include "kb_service_backend.h"

#include "aimee.h"
#include "modules/memory/memory_bus_context.h"
#include <errno.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Transitional transport for the Go owner's complete assertion result. */
cJSON *db2_kb_service_memory_search_assertions_json(const char *query, const char *valid_at,
                                                    const char *believed_at, int include_historical,
                                                    int max_hops, int limit)
{
   cJSON *request = cJSON_CreateObject(), *response = NULL;
   if (!request || !cJSON_AddStringToObject(request, "operation", "assertion-search") ||
       !cJSON_AddStringToObject(request, "query", query ? query : "") ||
       !cJSON_AddStringToObject(request, "valid_at", valid_at ? valid_at : "") ||
       !cJSON_AddStringToObject(request, "believed_at", believed_at ? believed_at : "") ||
       !cJSON_AddBoolToObject(request, "include_historical", include_historical) ||
       !cJSON_AddNumberToObject(request, "max_hops", max_hops) ||
       !cJSON_AddNumberToObject(request, "limit", limit) || memory_bus_add_context(request) != 0)
   {
      cJSON_Delete(request);
      return NULL;
   }
   int rc = aimee_module_commands_dispatch_internal("memory.runtime", request, &response);
   cJSON_Delete(request);
   if (rc <= 0 || !cJSON_IsObject(response))
   {
      cJSON_Delete(response);
      return NULL;
   }
   cJSON *assertions = cJSON_GetObjectItemCaseSensitive(response, "assertions");
   if (!cJSON_IsArray(assertions) || cJSON_GetArraySize(assertions) > 64)
   {
      cJSON_Delete(response);
      return NULL;
   }
   cJSON *item;
   cJSON_ArrayForEach(item, assertions)
   {
      const char *id = jo_cstr(item, "stable_id");
      char *end = NULL;
      errno = 0;
      long long parsed = strtoll(id, &end, 10);
      char canonical[32];
      snprintf(canonical, sizeof(canonical), "%lld", parsed);
      if (errno || !end || *end || parsed <= 0 || strcmp(id, canonical) ||
          !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item, "rendered")) ||
          !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(item, "historical")))
      {
         cJSON_Delete(response);
         return NULL;
      }
      /* cJSON parses numbers as doubles. Reinstall the Go owner's exact token
       * before this transitional response is serialized or copied again. */
      cJSON *exact = cJSON_CreateRaw(id);
      if (!exact || !cJSON_ReplaceItemInObjectCaseSensitive(item, "assertion_id", exact))
      {
         cJSON_Delete(exact);
         cJSON_Delete(response);
         return NULL;
      }
   }
   return response;
}

cJSON *db2_kb_service_memory_assemble_typed_context_json(const cJSON *req)
{
   cJSON *request = cJSON_Duplicate(req, 1), *response = NULL;
   if (!request)
      return NULL;
   cJSON_DeleteItemFromObjectCaseSensitive(request, "operation");
   if (!cJSON_AddStringToObject(request, "operation", "typed-context") ||
       memory_bus_add_context(request) != 0)
   {
      cJSON_Delete(request);
      return NULL;
   }
   int rc = aimee_module_commands_dispatch_internal("memory.runtime", request, &response);
   cJSON_Delete(request);
   const cJSON *json = cJSON_GetObjectItemCaseSensitive(response, "json");
   cJSON *result = NULL;
   if (rc > 0 && cJSON_IsString(json) && strlen(json->valuestring) <= 1024 * 1024)
   {
      cJSON *validated = cJSON_ParseWithOpts(json->valuestring, NULL, 1);
      if (cJSON_IsObject(validated))
         result = cJSON_CreateRaw(json->valuestring);
      cJSON_Delete(validated);
   }
   cJSON_Delete(response);
   return result;
}

/* CSS facts use the same Go owner; only request/receipt transport remains here. */
cJSON *db2_kb_service_css_conventions_json(const char *project, int sync)
{
   cJSON *request = cJSON_CreateObject(), *response = NULL;
   if (!request ||
       !cJSON_AddStringToObject(request, "operation",
                                sync ? "css-convention-sync" : "css-conventions") ||
       !cJSON_AddStringToObject(request, "project", project ? project : ""))
   {
      cJSON_Delete(request);
      return NULL;
   }
   int rc = aimee_module_commands_dispatch_internal("memory.runtime", request, &response);
   cJSON_Delete(request);
   const cJSON *json = cJSON_GetObjectItemCaseSensitive(response, "json");
   cJSON *result = NULL;
   if (rc > 0 && cJSON_IsString(json) && strlen(json->valuestring) <= 1024 * 1024)
   {
      cJSON *validated = cJSON_ParseWithOpts(json->valuestring, NULL, 1);
      if (cJSON_IsObject(validated))
         result = cJSON_CreateRaw(json->valuestring);
      cJSON_Delete(validated);
   }
   cJSON_Delete(response);
   return result;
}
