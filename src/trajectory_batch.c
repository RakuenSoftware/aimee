/* trajectory_batch.c: batch generation for replayable trajectory exports. */
#include "trajectory.h"

#include "agent_config.h"
#include "agent_eval.h"
#include "agent_exec.h"
#include "config.h"
#include "cJSON.h"
#include "modules/db1/interaction_events.h"
#include "platform_path.h"
#include "platform_process.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TRAJ_BATCH_MAX_TASKS 256

static void safe_name(const char *in, char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   size_t oi = 0;
   for (size_t i = 0; in && in[i] && oi + 1 < cap; i++)
   {
      unsigned char ch = (unsigned char)in[i];
      if (isalnum(ch) || ch == '-' || ch == '_')
         out[oi++] = (char)ch;
      else if (oi > 0 && out[oi - 1] != '-')
         out[oi++] = '-';
   }
   if (oi == 0 && cap > 5)
   {
      memcpy(out, "task", 4);
      oi = 4;
   }
   out[oi] = '\0';
}

const char *trajectory_toolset_for_index(const char *dist, int index)
{
   static const char *research[] = {"readonly", "current_code", "validate", NULL};
   static const char *coding[] = {"full_stack", "current_code", "validate", NULL};
   static const char *mixed[] = {"readonly", "current_code", "validate", "full_stack", NULL};
   const char *const *sets = NULL;
   if (!dist || !dist[0] || strcmp(dist, "research") == 0)
      sets = research;
   else if (strcmp(dist, "coding") == 0)
      sets = coding;
   else if (strcmp(dist, "mixed") == 0)
      sets = mixed;
   else
      return dist;

   int n = 0;
   while (sets[n])
      n++;
   if (n == 0)
      return "readonly";
   if (index < 0)
      index = -index;
   return sets[index % n];
}

static int load_jsonl_tasks(const char *path, eval_task_t *tasks, int max_tasks)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      return -1;
   char line[8192];
   int n = 0;
   while (fgets(line, sizeof(line), fp) && n < max_tasks)
   {
      char *p = line;
      while (isspace((unsigned char)*p))
         p++;
      if (!*p || *p == '#')
         continue;
      cJSON *root = cJSON_Parse(p);
      if (!root)
         continue;
      eval_task_t *t = &tasks[n];
      memset(t, 0, sizeof(*t));
      cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
      cJSON *prompt = cJSON_GetObjectItemCaseSensitive(root, "prompt");
      cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
      if (cJSON_IsString(name))
         snprintf(t->name, sizeof(t->name), "%s", name->valuestring);
      else
         snprintf(t->name, sizeof(t->name), "task-%d", n + 1);
      if (cJSON_IsString(prompt))
         snprintf(t->prompt, sizeof(t->prompt), "%s", prompt->valuestring);
      if (cJSON_IsString(role))
         snprintf(t->role, sizeof(t->role), "%s", role->valuestring);
      else
         snprintf(t->role, sizeof(t->role), "execute");

      cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "success_check");
      if (cJSON_IsObject(sc))
      {
         cJSON *type = cJSON_GetObjectItemCaseSensitive(sc, "type");
         cJSON *value = cJSON_GetObjectItemCaseSensitive(sc, "value");
         if (cJSON_IsString(type))
            snprintf(t->success_check_type, sizeof(t->success_check_type), "%s", type->valuestring);
         if (cJSON_IsString(value))
            snprintf(t->success_check_value, sizeof(t->success_check_value), "%s",
                     value->valuestring);
      }
      n++;
      cJSON_Delete(root);
   }
   fclose(fp);
   return n;
}

static int load_batch_tasks(const char *path, eval_task_t *tasks, int max_tasks)
{
   struct stat st;
   if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
      return agent_eval_load_tasks(path, tasks, max_tasks);
   return load_jsonl_tasks(path, tasks, max_tasks);
}

static int task_passed(const eval_task_t *task, const agent_result_t *result, int rc)
{
   if (rc != 0 || !result || !result->success)
      return 0;
   if (!task->success_check_type[0])
      return 1;
   if (strcmp(task->success_check_type, "contains") == 0)
      return result->response && strstr(result->response, task->success_check_value) != NULL;
   return result->success;
}

static char *task_payload(const eval_task_t *task, const char *toolset)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "content", task->prompt);
   cJSON_AddStringToObject(obj, "task", task->name);
   cJSON_AddStringToObject(obj, "toolset", toolset ? toolset : "");
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json;
}

static char *result_payload(const agent_result_t *result, int passed, const char *toolset)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "content", result->response ? result->response : "");
   cJSON_AddStringToObject(obj, "agent", result->agent_name);
   cJSON_AddStringToObject(obj, "toolset", toolset ? toolset : "");
   cJSON_AddBoolToObject(obj, "success", passed);
   cJSON_AddNumberToObject(obj, "turns", result->turns);
   cJSON_AddNumberToObject(obj, "tool_calls", result->tool_calls);
   if (result->error[0])
      cJSON_AddStringToObject(obj, "error", result->error);
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json;
}

static int write_text_file(const char *path, const char *text)
{
   FILE *fp = fopen(path, "w");
   if (!fp)
      return -1;
   if (text)
      fputs(text, fp);
   fputc('\n', fp);
   fclose(fp);
   return 0;
}

int trajectory_batch_run(agent_config_t *cfg, const trajectory_batch_opts_t *opts,
                         char **summary_json)
{
   if (!cfg || !opts || !opts->tasks_path || !opts->tasks_path[0] || !summary_json)
      return -1;
   *summary_json = NULL;

   eval_task_t tasks[TRAJ_BATCH_MAX_TASKS];
   int task_count = load_batch_tasks(opts->tasks_path, tasks, TRAJ_BATCH_MAX_TASKS);
   if (task_count <= 0)
      return -1;

   char out_dir[MAX_PATH_LEN];
   if (opts->out_dir && opts->out_dir[0])
      snprintf(out_dir, sizeof(out_dir), "%s", opts->out_dir);
   else
      snprintf(out_dir, sizeof(out_dir), "%s/trajectories", config_output_dir());
   if (platform_mkdir_p(out_dir, 0700) != 0)
      return -1;

   const char *saved_toolset = getenv("AIMEE_ACTIVE_TOOLSET");
   char saved_buf[128] = "";
   if (saved_toolset && saved_toolset[0])
      snprintf(saved_buf, sizeof(saved_buf), "%s", saved_toolset);

   cJSON *summary = cJSON_CreateObject();
   cJSON_AddStringToObject(summary, "status", "ok");
   cJSON_AddStringToObject(summary, "out_dir", out_dir);
   cJSON_AddStringToObject(summary, "toolset_dist",
                           opts->toolset_dist && opts->toolset_dist[0] ? opts->toolset_dist
                                                                       : "research");
   cJSON *written = cJSON_AddArrayToObject(summary, "written");
   cJSON *counts = cJSON_AddObjectToObject(summary, "toolsets");

   int failures = 0;
   for (int i = 0; i < task_count; i++)
   {
      const char *toolset = trajectory_toolset_for_index(opts->toolset_dist, i);
      platform_setenv("AIMEE_ACTIVE_TOOLSET", toolset ? toolset : "");

      char safe[128];
      safe_name(tasks[i].name, safe, sizeof(safe));
      char sid[160];
      snprintf(sid, sizeof(sid), "traj-%ld-%03d-%s", (long)getpid(), i + 1, safe);

      char *tp = task_payload(&tasks[i], toolset);
      (void)ie_record(sid, IE_USER_TURN, "user", tp ? tp : "{}", "ok");
      free(tp);

      agent_result_t ar;
      int rc = agent_run(cfg, tasks[i].role[0] ? tasks[i].role : "execute", NULL, tasks[i].prompt,
                         0, &ar);
      int passed = task_passed(&tasks[i], &ar, rc);
      char *rp = result_payload(&ar, passed, toolset);
      (void)ie_record(sid, IE_AGENT_TURN, "agent", rp ? rp : "{}", passed ? "ok" : "error");
      free(rp);

      char *traj = NULL;
      if (trajectory_export(sid, &opts->export_opts, &traj) != 0 || !traj)
      {
         failures++;
         free(ar.response);
         free(traj);
         continue;
      }

      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/%03d-%s.jsonl", out_dir, i + 1, safe);
      if (write_text_file(path, traj) != 0)
         failures++;
      else
      {
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "task", tasks[i].name);
         cJSON_AddStringToObject(item, "session_id", sid);
         cJSON_AddStringToObject(item, "path", path);
         cJSON_AddStringToObject(item, "toolset", toolset ? toolset : "");
         cJSON_AddBoolToObject(item, "success", passed);
         cJSON_AddItemToArray(written, item);
         cJSON *count = cJSON_GetObjectItemCaseSensitive(counts, toolset ? toolset : "");
         if (cJSON_IsNumber(count))
            cJSON_SetNumberValue(count, count->valueint + 1);
         else
            cJSON_AddNumberToObject(counts, toolset ? toolset : "", 1);
      }
      free(traj);
      free(ar.response);
   }

   platform_setenv("AIMEE_ACTIVE_TOOLSET", saved_buf);
   cJSON_AddNumberToObject(summary, "tasks", task_count);
   cJSON_AddNumberToObject(summary, "failures", failures);
   *summary_json = cJSON_PrintUnformatted(summary);
   cJSON_Delete(summary);
   return failures == 0 && *summary_json ? 0 : -1;
}
