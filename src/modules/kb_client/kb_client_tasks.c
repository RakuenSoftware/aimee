/* kb_client_tasks.c: kb_client wrappers for the task.* RPC family
 * (create, list, update_state, delete, add_edge, get_edges).  Split
 * out of kb_client_memory.c so both files stay under the per-file
 * line cap. */

#include "kb_client.h"
#include "cJSON.h"
#include "tasks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in kb_client.c. */
char *kb_v1_action_request(const char *method, cJSON *req);

static void kbc_task_row_from_json(cJSON *t, aimee_task_t *out)
{
   memset(out, 0, sizeof(*out));
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(t, "id");
   cJSON *pid_j = cJSON_GetObjectItemCaseSensitive(t, "parent_id");
   cJSON *title_j = cJSON_GetObjectItemCaseSensitive(t, "title");
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(t, "state");
   cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(t, "confidence");
   cJSON *created_j = cJSON_GetObjectItemCaseSensitive(t, "created_at");
   cJSON *updated_j = cJSON_GetObjectItemCaseSensitive(t, "updated_at");
   cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(t, "session_id");
   if (cJSON_IsNumber(id_j))
      out->id = (int64_t)id_j->valuedouble;
   if (cJSON_IsNumber(pid_j))
      out->parent_id = (int64_t)pid_j->valuedouble;
   if (cJSON_IsString(title_j))
      snprintf(out->title, sizeof(out->title), "%s", title_j->valuestring);
   if (cJSON_IsString(state_j))
      snprintf(out->state, sizeof(out->state), "%s", state_j->valuestring);
   if (cJSON_IsNumber(conf_j))
      out->confidence = conf_j->valuedouble;
   if (cJSON_IsString(created_j))
      snprintf(out->created_at, sizeof(out->created_at), "%s", created_j->valuestring);
   if (cJSON_IsString(updated_j))
      snprintf(out->updated_at, sizeof(out->updated_at), "%s", updated_j->valuestring);
   if (cJSON_IsString(sid_j))
      snprintf(out->session_id, sizeof(out->session_id), "%s", sid_j->valuestring);
}

int kb_client_task_create(const char *title, const char *session_id, int64_t parent_id,
                          aimee_task_t *out)
{
   if (!title || !out)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "title", title);
   if (session_id && session_id[0])
      cJSON_AddStringToObject(req, "session_id", session_id);
   cJSON_AddNumberToObject(req, "parent_id", (double)parent_id);
   char *json = kb_v1_action_request("task.create", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return -1;
   }
   cJSON *task = cJSON_GetObjectItemCaseSensitive(resp, "task");
   if (cJSON_IsObject(task))
      kbc_task_row_from_json(task, out);
   else
      memset(out, 0, sizeof(*out));
   cJSON_Delete(resp);
   return 0;
}

int kb_client_task_update_state(int64_t id, const char *state)
{
   if (!state)
      return -1;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   cJSON_AddStringToObject(req, "state", state);
   char *json = kb_v1_action_request("task.update_state", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_task_delete(int64_t id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "id", (double)id);
   char *json = kb_v1_action_request("task.delete", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_task_add_edge(int64_t source, int64_t target, const char *relation)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "source", (double)source);
   cJSON_AddNumberToObject(req, "target", (double)target);
   if (relation && relation[0])
      cJSON_AddStringToObject(req, "relation", relation);
   char *json = kb_v1_action_request("task.add_edge", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int rc = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

int kb_client_task_get_edges(int64_t task_id, task_edge_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "task_id", (double)task_id);
   cJSON_AddNumberToObject(req, "max", max);
   char *json = kb_v1_action_request("task.get_edges", req);
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
   cJSON *edges = cJSON_GetObjectItemCaseSensitive(resp, "edges");
   int n = 0;
   if (cJSON_IsArray(edges))
   {
      cJSON *e;
      cJSON_ArrayForEach(e, edges)
      {
         if (n >= max)
            break;
         memset(&out[n], 0, sizeof(out[n]));
         cJSON *id_j = cJSON_GetObjectItemCaseSensitive(e, "id");
         cJSON *src_j = cJSON_GetObjectItemCaseSensitive(e, "source_id");
         cJSON *dst_j = cJSON_GetObjectItemCaseSensitive(e, "target_id");
         cJSON *rel_j = cJSON_GetObjectItemCaseSensitive(e, "relation");
         if (cJSON_IsNumber(id_j))
            out[n].id = (int64_t)id_j->valuedouble;
         if (cJSON_IsNumber(src_j))
            out[n].source_id = (int64_t)src_j->valuedouble;
         if (cJSON_IsNumber(dst_j))
            out[n].target_id = (int64_t)dst_j->valuedouble;
         if (cJSON_IsString(rel_j))
            snprintf(out[n].relation, sizeof(out[n].relation), "%s", rel_j->valuestring);
         n++;
      }
   }
   cJSON_Delete(resp);
   return n;
}

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
