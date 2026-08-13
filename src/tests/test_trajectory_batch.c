/* test_trajectory_batch.c: batch trajectory generation tests. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aimee.h"
#include "agent_eval.h"
#include "agent_exec.h"
#include "cJSON.h"
#include "db1.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "trajectory.h"

int agent_eval_load_tasks(const char *suite_dir, eval_task_t *tasks, int max_tasks)
{
   (void)suite_dir;
   (void)tasks;
   (void)max_tasks;
   return 0;
}

int agent_run(agent_config_t *cfg, const char *role, const char *system_prompt,
              const char *user_prompt, int max_tokens, agent_result_t *out)
{
   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)max_tokens;
   memset(out, 0, sizeof(*out));
   out->success = 1;
   if (user_prompt && strstr(user_prompt, "secret"))
      out->response = strdup("api_key = sk-testtrajectorysecret");
   else
      out->response = strdup(user_prompt && strstr(user_prompt, "second") ? "second done" : "done");
   snprintf(out->agent_name, sizeof(out->agent_name), "unit-agent");
   out->turns = 1;
   return 0;
}

static void write_text(const char *path, const char *text)
{
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   fputs(text, fp);
   fclose(fp);
}

static void test_batch_writes_one_trajectory_per_task(void)
{
   char db_path[256];
   snprintf(db_path, sizeof db_path, "%s/test_trajectory_batch_XXXXXX.db", platform_tmpdir());
   int fd = mkstemps(db_path, 3);
   if (fd >= 0)
      close(fd);
   platform_test_remove_sqlite(db_path);
   assert(db1_init(db_path) == 0);

   char out_dir[256];
   snprintf(out_dir, sizeof(out_dir), "/tmp/aimee-traj-batch-%ld", (long)getpid());
   platform_mkdir_p(out_dir, 0700);
   char tasks_path[256];
   snprintf(tasks_path, sizeof(tasks_path), "%s/tasks.jsonl", out_dir);
   write_text(tasks_path, "{\"name\":\"first\",\"prompt\":\"say done\",\"role\":\"execute\","
                          "\"success_check\":{\"type\":\"contains\",\"value\":\"done\"}}\n"
                          "{\"name\":\"second\",\"prompt\":\"second task\",\"role\":\"execute\","
                          "\"success_check\":{\"type\":\"contains\",\"value\":\"second\"}}\n");

   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   trajectory_batch_opts_t opts = {
       .tasks_path = tasks_path,
       .toolset_dist = "research",
       .out_dir = out_dir,
       .export_opts = {.compress = 1, .redact = 1, .max_tool_result_bytes = 64}};
   char *summary = NULL;
   assert(trajectory_batch_run(&cfg, &opts, &summary) == 0);
   cJSON *root = cJSON_Parse(summary);
   assert(root != NULL);
   assert(cJSON_GetObjectItem(root, "tasks")->valueint == 2);
   cJSON *written = cJSON_GetObjectItem(root, "written");
   assert(cJSON_IsArray(written));
   assert(cJSON_GetArraySize(written) == 2);
   cJSON *counts = cJSON_GetObjectItem(root, "toolsets");
   assert(cJSON_GetObjectItem(counts, "readonly")->valueint == 1);
   assert(cJSON_GetObjectItem(counts, "current_code")->valueint == 1);
   cJSON *first = cJSON_GetArrayItem(written, 0);
   cJSON *second = cJSON_GetArrayItem(written, 1);
   assert(access(cJSON_GetObjectItem(first, "path")->valuestring, F_OK) == 0);
   assert(access(cJSON_GetObjectItem(second, "path")->valuestring, F_OK) == 0);
   cJSON_Delete(root);
   free(summary);

   db1_shutdown();
   platform_test_remove_sqlite(db_path);
   printf("  PASS: test_batch_writes_one_trajectory_per_task\n");
}

static void test_batch_refuses_unresolved_secret_without_file(void)
{
   char db_path[256];
   snprintf(db_path, sizeof db_path, "%s/test_trajectory_batch_secret_XXXXXX.db",
            platform_tmpdir());
   int fd = mkstemps(db_path, 3);
   if (fd >= 0)
      close(fd);
   platform_test_remove_sqlite(db_path);
   assert(db1_init(db_path) == 0);

   char out_dir[256];
   snprintf(out_dir, sizeof(out_dir), "/tmp/aimee-traj-batch-secret-%ld", (long)getpid());
   platform_test_rmrf(out_dir);
   platform_mkdir_p(out_dir, 0700);
   char tasks_path[256];
   snprintf(tasks_path, sizeof(tasks_path), "%s/tasks.jsonl", out_dir);
   write_text(tasks_path,
              "{\"name\":\"secret task\",\"prompt\":\"secret task\",\"role\":\"execute\"}\n");

   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   trajectory_batch_opts_t opts = {
       .tasks_path = tasks_path,
       .toolset_dist = "research",
       .out_dir = out_dir,
       .export_opts = {
           .compress = 1, .redact = 1, .max_tool_result_bytes = 64, .redaction_buf_bytes = 8}};
   char *summary = NULL;
   assert(trajectory_batch_run(&cfg, &opts, &summary) != 0);
   assert(summary != NULL);
   cJSON *root = cJSON_Parse(summary);
   assert(root != NULL);
   assert(cJSON_GetObjectItem(root, "tasks")->valueint == 1);
   assert(cJSON_GetObjectItem(root, "failures")->valueint == 1);
   cJSON *written = cJSON_GetObjectItem(root, "written");
   assert(cJSON_IsArray(written));
   assert(cJSON_GetArraySize(written) == 0);
   char output_path[256];
   snprintf(output_path, sizeof(output_path), "%s/001-secret-task.jsonl", out_dir);
   assert(access(output_path, F_OK) != 0);
   cJSON_Delete(root);
   free(summary);

   db1_shutdown();
   platform_test_remove_sqlite(db_path);
   platform_test_rmrf(out_dir);
   printf("  PASS: test_batch_refuses_unresolved_secret_without_file\n");
}

int main(void)
{
   printf("trajectory_batch:\n");
   test_batch_writes_one_trajectory_per_task();
   test_batch_refuses_unresolved_secret_without_file();
   printf("trajectory_batch: all tests passed\n");
   return 0;
}
