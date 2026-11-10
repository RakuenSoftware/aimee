/* Legacy session-briefing, task and graph services awaiting module cutover. */

#include "kb_service_backend.h"

#include "aimee.h"
#include "json_fluent.h"
#include "config.h"
#include "modules/kb/c/kb_payload.h" /* kb_store_kb_async_enqueue */
#include "modules/kb/c/decision_log.h"
#include "session_briefing.h"
#include "modules/kb/c/tasks.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

cJSON *kb_store_kb_service_task_create_json(const char *title, const char *session_id,
                                            int64_t parent_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   aimee_task_t task;
   memset(&task, 0, sizeof(task));
   if (kb_store_task_create(title ? title : "", session_id ? session_id : "", parent_id, &task) !=
       0)
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

cJSON *kb_store_kb_service_task_update_state_json(int64_t id, const char *state)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = kb_store_task_update_state(id, state ? state : "");
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to update task state");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *kb_store_kb_service_task_delete_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = kb_store_task_delete(id);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to delete task");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *kb_store_kb_service_task_add_edge_json(int64_t source, int64_t target, const char *relation)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = kb_store_task_add_edge(source, target, relation ? relation : "depends_on");
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to add task edge");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *kb_store_kb_service_task_get_edges_json(int64_t task_id, int max)
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
   int n = kb_store_task_get_edges(task_id, edges, max);
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

cJSON *kb_store_kb_service_task_list_json(const char *state, const char *session_id, int limit)
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
   int n = kb_store_task_list((state && state[0]) ? state : NULL,
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

/* A scope term: a run of [a-z0-9_./-] of at least 3 characters that is not a
 * filler word. Lower-cases into term; returns the input position after it. */
static const char *task_next_term(const char *p, char *term, size_t cap)
{
   static const char *const filler[] = {"the",  "and",  "for",  "with", "from",
                                        "into", "task", "this", "that", NULL};
   for (;;)
   {
      while (*p && !(isalnum((unsigned char)*p) || strchr("_./-", *p)))
         p++;
      if (!*p)
         return NULL;
      size_t n = 0;
      while (*p && (isalnum((unsigned char)*p) || strchr("_./-", *p)))
      {
         if (n + 1 < cap)
            term[n++] = (char)tolower((unsigned char)*p);
         p++;
      }
      term[n] = '\0';
      int skip = n < 3;
      for (int i = 0; !skip && filler[i]; i++)
         skip = !strcmp(term, filler[i]);
      if (!skip)
         return p;
   }
}

int kb_task_scope_overlaps(const char *const *titles, int n, const char *file_path,
                           const char *command)
{
   file_path = file_path ? file_path : "";
   command = command ? command : "";
   if (!file_path[0] && !command[0])
      return 1;
   size_t len = strlen(file_path) + strlen(command) + 2;
   char *target = malloc(len);
   if (!target)
      return 1;
   snprintf(target, len, "%s %s", file_path, command);
   for (char *c = target; *c; c++)
      *c = (char)tolower((unsigned char)*c);
   int overlaps = 0;
   char term[256];
   for (int i = 0; i < n && !overlaps; i++)
      for (const char *p = titles[i]; !overlaps && (p = task_next_term(p, term, sizeof(term)));)
         overlaps = strstr(target, term) != NULL;
   free(target);
   return overlaps;
}

cJSON *kb_store_kb_service_task_check_drift_json(int64_t task_id, const char *file_path,
                                                 const char *command)
{
   aimee_task_t task;
   if (task_id <= 0 || kb_store_task_get(task_id, &task) != 0)
      return jo_ok();
   aimee_task_t children[32];
   int n = kb_store_task_children(task_id, children, 32);
   if (n < 0)
      return NULL;
   const char *titles[33] = {task.title};
   for (int i = 0; i < n; i++)
      titles[i + 1] = children[i].title;
   int drifted = !kb_task_scope_overlaps(titles, n + 1, file_path, command);
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "task_id", (double)task_id);
   cJSON_AddStringToObject(resp, "task_title", task.title);
   cJSON_AddBoolToObject(resp, "drifted", drifted);
   char message[400];
   if (drifted)
      snprintf(message, sizeof(message),
               "The requested file or command does not overlap the active task scope: %s",
               task.title);
   else
      snprintf(message, sizeof(message), "Action overlaps the active task scope");
   cJSON_AddStringToObject(resp, "message", message);
   return resp;
}
