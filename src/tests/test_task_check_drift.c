/* task.check_drift: a file or command drifts when it shares no scope term with
 * the task's title or its subtasks' titles (the 1.0 memory.check_drift rule). */
#include "aimee.h"
#include "cJSON.h"
#include "modules/kb/c/kb_service_backend.h"
#include "modules/kb/c/tasks.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int children_fail;

int kb_store_task_get(int64_t id, aimee_task_t *out)
{
   if (id != 7)
      return -1;
   memset(out, 0, sizeof(*out));
   out->id = 7;
   snprintf(out->title, sizeof(out->title), "Fix the parser for config files");
   return 0;
}
int kb_store_task_children(int64_t parent_id, aimee_task_t *out, int max)
{
   assert(parent_id == 7 && max == 32);
   if (children_fail)
      return -1;
   memset(out, 0, sizeof(*out));
   snprintf(out[0].title, sizeof(out[0].title), "Update src/lexer.c");
   return 1;
}
int kb_store_task_create(const char *t, const char *s, int64_t p, aimee_task_t *o)
{
   return -1;
}
int kb_store_task_delete(int64_t id)
{
   return -1;
}
int kb_store_task_update_state(int64_t id, const char *state)
{
   return -1;
}
int kb_store_task_list(const char *st, const char *sid, int limit, aimee_task_t *out, int max)
{
   return 0;
}
int kb_store_task_add_edge(int64_t s, int64_t t, const char *r)
{
   return -1;
}
int kb_store_task_get_edges(int64_t id, task_edge_t *out, int max)
{
   return 0;
}

static int drifted(const char *file, const char *command)
{
   cJSON *r = kb_store_kb_service_task_check_drift_json(7, file, command);
   assert(r && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "status")), "ok"));
   assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "task_id")) == 7);
   int d = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "drifted"));
   const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "message"));
   assert(message && (d ? strstr(message, "Fix the parser") != NULL
                        : !strcmp(message, "Action overlaps the active task scope")));
   cJSON_Delete(r);
   return d;
}

int main(void)
{
   assert(!drifted("src/Parser.c", ""));
   assert(!drifted("", "make CONFIG=1"));
   assert(!drifted("src/lexer.c", "")); /* a subtask term counts */
   assert(drifted("docs/readme.md", "ls"));
   /* Filler words and short terms never establish overlap. */
   assert(drifted("other/format", ""));
   assert(!drifted("", ""));

   cJSON *unknown = kb_store_kb_service_task_check_drift_json(8, "x", "");
   assert(unknown && !cJSON_GetObjectItemCaseSensitive(unknown, "drifted"));
   cJSON_Delete(unknown);
   children_fail = 1;
   assert(!kb_store_kb_service_task_check_drift_json(7, "x", ""));
   printf("task.check_drift: ok\n");
   return 0;
}
