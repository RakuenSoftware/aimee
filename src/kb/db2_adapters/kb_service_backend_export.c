/* kb/db2_adapters/kb_service_backend_export.c: caller-side filtered memory
 * export/import composition for kb.export / kb.import RPCs. */

#include "kb_service_backend_export.h"
#include "module_commands.h"
#include "kb_service.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

cJSON *db2_kb_service_memory_export_filtered_json(const char *workspace, const char *kind,
                                                  const char *since_iso, int include_archived)
{
   cJSON *request = cJSON_CreateObject(), *response = NULL;
   if (!request)
      return NULL;
   cJSON_AddStringToObject(request, "operation", "export-filtered");
   if (workspace)
      cJSON_AddStringToObject(request, "workspace", workspace);
   if (kind)
      cJSON_AddStringToObject(request, "kind", kind);
   if (since_iso)
      cJSON_AddStringToObject(request, "since", since_iso);
   cJSON_AddBoolToObject(request, "include_archived", include_archived);
   cJSON *context = kb_service_command_context();
   if (!context)
   {
      cJSON_Delete(request);
      return NULL;
   }
   int rc = aimee_module_commands_dispatch_internal_context_timeout("memory.runtime", request,
                                                                    context, 60000, &response);
   cJSON_Delete(context);
   cJSON_Delete(request);
   const char *raw = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "json"));
   cJSON *result = rc > 0 && raw ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
   cJSON_Delete(response);
   if (!cJSON_IsObject(result))
   {
      cJSON_Delete(result);
      return NULL;
   }
   return result;
}

int db2_kb_service_memory_import_json(cJSON *memories_arr, const char *workspace_override,
                                      int dry_run, int *imported_out)
{
   if (imported_out)
      *imported_out = 0;
   if (!cJSON_IsArray(memories_arr))
      return -1;

   int count = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, memories_arr)
   {
      cJSON *tier_j = cJSON_GetObjectItemCaseSensitive(item, "tier");
      cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(item, "kind");
      cJSON *epistemic_j = cJSON_GetObjectItemCaseSensitive(item, "epistemic_kind");
      cJSON *key_j = cJSON_GetObjectItemCaseSensitive(item, "key");
      cJSON *content_j = cJSON_GetObjectItemCaseSensitive(item, "content");
      cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(item, "confidence");
      cJSON *sess_j = cJSON_GetObjectItemCaseSensitive(item, "source_session");

      if (!cJSON_IsString(kind_j) || !cJSON_IsString(key_j) || !cJSON_IsString(content_j))
         continue; /* skip malformed */

      const char *tier = cJSON_IsString(tier_j) ? tier_j->valuestring : "L0";
      double confidence = cJSON_IsNumber(conf_j) ? conf_j->valuedouble : 1.0;
      const char *session_id = cJSON_IsString(sess_j) ? sess_j->valuestring : "";

      if (!dry_run)
      {
         cJSON *request = cJSON_CreateObject(), *r = NULL;
         if (!request)
            return -1;
         cJSON_AddStringToObject(request, "tier", tier);
         cJSON_AddStringToObject(request, "kind", kind_j->valuestring);
         if (epistemic_j)
            cJSON_AddItemToObject(request, "epistemic_kind", cJSON_Duplicate(epistemic_j, 1));
         cJSON_AddStringToObject(request, "key", key_j->valuestring);
         cJSON_AddStringToObject(request, "content", content_j->valuestring);
         cJSON_AddNumberToObject(request, "confidence", confidence);
         cJSON_AddStringToObject(request, "session_id", session_id);
         if (workspace_override && workspace_override[0])
         {
            cJSON_AddBoolToObject(request, "scope_context", 1);
            cJSON_AddStringToObject(request, "workspace", workspace_override);
         }
         cJSON *context = kb_service_command_context();
         if (!context)
         {
            cJSON_Delete(request);
            return -1;
         }
         (void)aimee_module_commands_dispatch_context("memory.store", request, context, &r);
         cJSON_Delete(context);
         cJSON_Delete(request);
         const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "status"));
         int ok = status && strcmp(status, "ok") == 0;
         cJSON_Delete(r);
         if (!ok)
            return -1;
      }
      count++;
   }

   if (imported_out)
      *imported_out = count;
   return 0;
}
