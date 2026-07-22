/* kb_client_data.c: kb_client wrappers for the export/import path —
 * memory.export_jsonl, memory.decisions_export_jsonl, memory.key_exists.
 * Split out of kb_client_memory.c so the two stay under the per-file
 * line cap. */

#include "kb_client.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* Defined in kb_client.c. */
char *kb_v1_action_request(const char *method, cJSON *req);

static int kbc_count_envelope_data(const char *json)
{
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *count = cJSON_GetObjectItemCaseSensitive(resp, "count");
   int rc = -1;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsNumber(count))
      rc = (int)count->valuedouble;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_memory_export_jsonl(const char *path)
{
   if (!path)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "path", path);
   char *json = kb_v1_action_request("memory.export_jsonl", req);
   int rc = kbc_count_envelope_data(json);
   free(json);
   return rc;
}

int kb_client_memory_decisions_export_jsonl(const char *path)
{
   if (!path)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "path", path);
   char *json = kb_v1_action_request("memory.decisions_export_jsonl", req);
   int rc = kbc_count_envelope_data(json);
   free(json);
   return rc;
}

int kb_client_memory_key_exists(const char *key)
{
   if (!key)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "key", key);
   char *json = kb_v1_action_request("memory.key_exists", req);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *exists = cJSON_GetObjectItemCaseSensitive(resp, "exists");
   int rc = 0;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsBool(exists) &&
       cJSON_IsTrue(exists))
      rc = 1;
   cJSON_Delete(resp);
   return rc;
}
