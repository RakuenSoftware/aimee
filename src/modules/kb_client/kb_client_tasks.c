/* kb_client_tasks.c: kb_client wrappers for the task.* RPC family
 * (create, list, update_state, delete, add_edge, get_edges).  Split
 * out of kb_client_memory.c so both files stay under the per-file
 * line cap. */

#include "kb_client.h"
#include "kb_client_pii.h"
#include "cJSON.h"
#include "tasks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in kb_client.c. */
char *kb_v1_action_request(const char *method, cJSON *req);

int kb_client_task_list(const char *state, const char *session_id, int limit, aimee_task_t *out,
                        int max)
{
   if (!out || max <= 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   if (state && state[0])
      cJSON_AddStringToObject(req, "state", state);
   if (session_id && session_id[0])
      cJSON_AddStringToObject(req, "session_id", session_id);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   char *json = kb_v1_action_request("task.list", req);
   if (!json)
      return 0;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return 0;
   }
   cJSON *tasks = cJSON_GetObjectItemCaseSensitive(resp, "tasks");
   int n = 0;
   if (cJSON_IsArray(tasks))
   {
      cJSON *t;
      cJSON_ArrayForEach(t, tasks)
      {
         if (n >= max)
            break;
         memset(&out[n], 0, sizeof(out[n]));
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(t, "id");
         cJSON *pid_j = cJSON_GetObjectItemCaseSensitive(t, "parent_id");
         cJSON *title_j = cJSON_GetObjectItemCaseSensitive(t, "title");
         cJSON *state_j = cJSON_GetObjectItemCaseSensitive(t, "state");
         cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(t, "confidence");
         cJSON *created_j = cJSON_GetObjectItemCaseSensitive(t, "created_at");
         cJSON *updated_j = cJSON_GetObjectItemCaseSensitive(t, "updated_at");
         cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(t, "session_id");
         if (cJSON_IsNumber(id_j))
            out[n].id = (int64_t)id_j->valuedouble;
         if (cJSON_IsNumber(pid_j))
            out[n].parent_id = (int64_t)pid_j->valuedouble;
         if (cJSON_IsString(title_j))
            snprintf(out[n].title, sizeof(out[n].title), "%s", title_j->valuestring);
         if (cJSON_IsString(state_j))
            snprintf(out[n].state, sizeof(out[n].state), "%s", state_j->valuestring);
         if (cJSON_IsNumber(conf_j))
            out[n].confidence = conf_j->valuedouble;
         if (cJSON_IsString(created_j))
            snprintf(out[n].created_at, sizeof(out[n].created_at), "%s", created_j->valuestring);
         if (cJSON_IsString(updated_j))
            snprintf(out[n].updated_at, sizeof(out[n].updated_at), "%s", updated_j->valuestring);
         if (cJSON_IsString(sid_j))
            snprintf(out[n].session_id, sizeof(out[n].session_id), "%s", sid_j->valuestring);
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}
