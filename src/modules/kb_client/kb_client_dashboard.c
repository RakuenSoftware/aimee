/* kb_client_dashboard.c: kb_client wrappers for dashboard.* RPCs */
#include "kb_client.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* Defined in kb_client.c. */
char *kb_v1_action_request(const char *method, cJSON *req);

char *kb_client_dashboard_memory_stats_json(void)
{
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request("dashboard.memory_stats", req);
   if (!json)
      return NULL;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *payload = cJSON_GetObjectItemCaseSensitive(resp, "payload");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 ||
       !cJSON_IsObject(payload))
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON *detached = cJSON_DetachItemViaPointer(resp, payload);
   char *out = detached ? cJSON_PrintUnformatted(detached) : NULL;
   cJSON_Delete(detached);
   cJSON_Delete(resp);
   return out;
}

static char *kb_client_v1_dashboard_payload_request(const char *method)
{
   cJSON *req = cJSON_CreateObject();
   char *json = kb_v1_action_request(method, req);
   if (!json)
      return NULL;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return NULL;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *payload = cJSON_GetObjectItemCaseSensitive(resp, "payload");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !payload)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON *detached = cJSON_DetachItemViaPointer(resp, payload);
   char *out = detached ? cJSON_PrintUnformatted(detached) : NULL;
   cJSON_Delete(detached);
   cJSON_Delete(resp);
   return out;
}

char *kb_client_dashboard_logs_json(void)
{
   return kb_client_v1_dashboard_payload_request("dashboard.logs");
}

char *kb_client_dashboard_reminders_json(void)
{
   return kb_client_v1_dashboard_payload_request("dashboard.reminders");
}

char *kb_client_dashboard_recall_json(void)
{
   return kb_client_v1_dashboard_payload_request("dashboard.recall");
}

char *kb_client_dashboard_directives_json(void)
{
   return kb_client_v1_dashboard_payload_request("dashboard.directives");
}
