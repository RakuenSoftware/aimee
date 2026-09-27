#include "module_commands.h"
#include "json_fluent.h"
/* Transitional CSS transport. */

#include "kb/kb_service_css.h"

#include "aimee.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
