/* tasks.c: high-level task helpers that compose data from multiple
 * stores. Storage primitives live in db1/db2; this file just wraps
 * the snapshot composition (checkpoint_create) and the restore path
 * (which writes into memory). */
#include "aimee.h"
#include "tasks_compose.h"
#include "kb_client.h"
#include "cJSON.h"
#include "json_fluent.h"

int tasks_checkpoint_create(const char *label, const char *session_id, int64_t task_id,
                            db1_checkpoint_t *out)
{
   if (!label || !label[0] || !out)
      return -1;

   cJSON *snap = cJSON_CreateObject();
   if (!snap)
      return -1;

   /* Active tasks (DB2 via aimee-kb) */
   {
      cJSON *arr = cJSON_AddArrayToObject(snap, "tasks");
      aimee_task_t tasks[32];
      int tc = kb_client_task_list(TASK_IN_PROGRESS, NULL, 32, tasks, 32);
      if (tc < 0)
      {
         cJSON_Delete(snap);
         return -1;
      }
      for (int i = 0; i < tc; i++)
      {
         cJSON *t = cJSON_CreateObject();
         cJSON_AddNumberToObject(t, "id", (double)tasks[i].id);
         JSON_ADD_STR(t, "title", tasks[i].title);
         JSON_ADD_STR(t, "state", tasks[i].state);
         cJSON_AddItemToArray(arr, t);
      }
   }

   /* The Go owner selects and serializes the checkpoint's memory projection. */
   {
      cJSON *args = cJSON_CreateObject();
      if (!args)
      {
         cJSON_Delete(snap);
         return -1;
      }
      cJSON_AddStringToObject(args, "action", "facts");
      kb_client_memory_scope_context_apply(args);
      char *raw = kb_v1_action_request("memory.checkpoint", args);
      cJSON *response = raw ? cJSON_Parse(raw) : NULL;
      free(raw);
      cJSON *facts = response ? cJSON_DetachItemFromObjectCaseSensitive(response, "facts") : NULL;
      if (!response || strcmp(jo_cstr(response, "status"), "ok") || !cJSON_IsArray(facts))
      {
         cJSON_Delete(facts);
         cJSON_Delete(response);
         cJSON_Delete(snap);
         return -1;
      }
      cJSON_AddItemToObject(snap, "facts", facts);
      cJSON_Delete(response);
   }

   /* Recent decisions (DB2 via aimee-kb) */
   {
      cJSON *arr = cJSON_AddArrayToObject(snap, "decisions");
      db2_decision_log_row_t decs[8];
      int dc = kb_client_decision_log_list(NULL, 8, decs, 8);
      if (dc < 0)
      {
         cJSON_Delete(snap);
         return -1;
      }
      for (int i = 0; i < dc; i++)
      {
         cJSON *d = cJSON_CreateObject();
         JSON_ADD_STR(d, "chosen", decs[i].chosen);
         JSON_ADD_STR(d, "rationale", decs[i].rationale);
         cJSON_AddItemToArray(arr, d);
      }
   }

   char *snap_str = cJSON_PrintUnformatted(snap);
   cJSON_Delete(snap);
   if (!snap_str)
      return -1;

   /* The DB1 checkpoint ABI must be able to return the complete snapshot. */
   if (strlen(snap_str) >= sizeof(out->snapshot))
   {
      free(snap_str);
      return -1;
   }
   int rc = db1_checkpoint_insert(label, session_id, task_id, snap_str, out);
   free(snap_str);
   return rc;
}

int tasks_checkpoint_restore(int64_t id, const char *session_id)
{
   db1_checkpoint_t cp;
   if (db1_checkpoint_get(id, &cp) != 0)
      return -1;

   cJSON *args = cJSON_CreateObject();
   if (!args)
      return -1;
   char checkpoint_id[32];
   snprintf(checkpoint_id, sizeof(checkpoint_id), "%lld", (long long)id);
   cJSON_AddStringToObject(args, "action", "restore");
   cJSON_AddStringToObject(args, "checkpoint_id", checkpoint_id);
   cJSON_AddStringToObject(args, "snapshot", cp.snapshot);
   cJSON_AddStringToObject(args, "session_id", session_id ? session_id : "");
   kb_client_memory_scope_context_apply(args);
   char *raw = kb_v1_action_request("memory.checkpoint", args);
   cJSON *response = raw ? cJSON_Parse(raw) : NULL;
   free(raw);
   int rc =
       response && !strcmp(jo_cstr(response, "status"), "ok") && jo_cstr(response, "memory_id")[0]
           ? 0
           : -1;
   cJSON_Delete(response);
   return rc;
}
