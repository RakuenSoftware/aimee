/* Legacy session-briefing, task and graph services awaiting module cutover. */

#include "kb_service_backend.h"

#include "aimee.h"
#include "config.h"
#include "modules/db2/c/kb_payload.h" /* db2_kb_async_enqueue */
#include "modules/db2/c/decision_log.h"
#include "session_briefing.h"
#include "modules/db2/c/tasks.h"

#include <stdlib.h>
#include <string.h>

cJSON *db2_kb_service_session_briefing_commitments_json(int limit)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = session_briefing_render_commitments(limit);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "body", body ? body : "");
   free(body);
   return resp;
}

cJSON *db2_kb_service_session_briefing_directives_json(int limit)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = session_briefing_render_directives(limit);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "body", body ? body : "");
   free(body);
   return resp;
}

/* Read-only typed-fact recall (§7), PII-gated: the cheap path ingress_preinject
 * calls every turn. No write; facts="" when there are none. */

static cJSON *kbs_task_row_to_json(const aimee_task_t *t)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)t->id);
   cJSON_AddNumberToObject(obj, "parent_id", (double)t->parent_id);
   cJSON_AddStringToObject(obj, "title", t->title);
   cJSON_AddStringToObject(obj, "state", t->state);
   cJSON_AddNumberToObject(obj, "confidence", t->confidence);
   cJSON_AddStringToObject(obj, "created_at", t->created_at);
   cJSON_AddStringToObject(obj, "updated_at", t->updated_at);
   cJSON_AddStringToObject(obj, "session_id", t->session_id);
   return obj;
}

cJSON *db2_kb_service_task_create_json(const char *title, const char *session_id, int64_t parent_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   aimee_task_t task;
   memset(&task, 0, sizeof(task));
   if (db2_task_create(title ? title : "", session_id ? session_id : "", parent_id, &task) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to create task");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *obj = kbs_task_row_to_json(&task);
   if (obj)
      cJSON_AddItemToObject(resp, "task", obj);
   return resp;
}

cJSON *db2_kb_service_task_update_state_json(int64_t id, const char *state)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_task_update_state(id, state ? state : "");
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to update task state");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_task_delete_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_task_delete(id);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to delete task");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_task_add_edge_json(int64_t source, int64_t target, const char *relation)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_task_add_edge(source, target, relation ? relation : "depends_on");
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to add task edge");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_task_get_edges_json(int64_t task_id, int max)
{
   if (max < 1)
      max = 16;
   if (max > 64)
      max = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "edges") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   task_edge_t edges[64];
   int n = db2_task_get_edges(task_id, edges, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *e = cJSON_CreateObject();
      if (!e)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(e, "id", (double)edges[i].id);
      cJSON_AddNumberToObject(e, "source_id", (double)edges[i].source_id);
      cJSON_AddNumberToObject(e, "target_id", (double)edges[i].target_id);
      cJSON_AddStringToObject(e, "relation", edges[i].relation);
      cJSON_AddItemToArray(arr, e);
   }
   return resp;
}

cJSON *db2_kb_service_task_list_json(const char *state, const char *session_id, int limit)
{
   if (limit < 1)
      limit = 16;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "tasks") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   aimee_task_t rows[64];
   int n = db2_task_list((state && state[0]) ? state : NULL,
                         (session_id && session_id[0]) ? session_id : NULL, limit, rows, 64);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(obj, "id", (double)rows[i].id);
      cJSON_AddNumberToObject(obj, "parent_id", (double)rows[i].parent_id);
      cJSON_AddStringToObject(obj, "title", rows[i].title);
      cJSON_AddStringToObject(obj, "state", rows[i].state);
      cJSON_AddNumberToObject(obj, "confidence", rows[i].confidence);
      cJSON_AddStringToObject(obj, "created_at", rows[i].created_at);
      cJSON_AddStringToObject(obj, "updated_at", rows[i].updated_at);
      cJSON_AddStringToObject(obj, "session_id", rows[i].session_id);
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}
