/* test_trajectory_batch.c: batch trajectory generation tests. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aimee.h"
#include "agent_eval.h"
#include "agent_exec.h"
#include "db1_client/server_sessions.h"
#include "db1_client/primary_sessions.h"
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include "db1_client/db1.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "trajectory.h"
#include "support/store_module_fixture.h"
#include "support/module_runtime_fixture.h"

/* Export screening uses the same Go owner as production. The pipe supplies
 * transport only; no PII policy is duplicated in this fixture. */
int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.screen_content") == 0);
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "fixture_command", "screen_content");
   cJSON_AddItemToObject(request, "fixture_arguments", cJSON_Duplicate(args, 1));
   int rc = module_runtime_fixture_call(request, result);
   cJSON_Delete(request);
   return rc;
}

int agent_eval_load_tasks(const char *suite_dir, eval_task_t *tasks, int max_tasks)
{
   (void)suite_dir;
   (void)tasks;
   (void)max_tasks;
   return 0;
}

static _Thread_local char active_toolset[128];
void agent_tools_set_active_toolset(const char *name)
{
   snprintf(active_toolset, sizeof active_toolset, "%s", name ? name : "");
}
const char *agent_tools_active_toolset(void)
{
   return active_toolset[0] ? active_toolset : NULL;
}

static const char *attested_session = "fixture-parent";
const char *agent_get_request_session(void)
{
   return attested_session;
}

const char *agent_get_request_vault_principal(void)
{
   return "trajectory-fixture-owner";
}

int agent_run(agent_config_t *cfg, const char *role, const aimee_request_t *request,
              agent_result_t *out)
{
   const char *system_prompt = request->n_system ? request->system[0].text : NULL;
   const char *user_prompt = request->n_messages ? request->messages[0].blocks[0].text : NULL;
   int max_tokens = request->has_max_tokens ? request->max_tokens : 0;

   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)max_tokens;
   static int serial;
   assert(active_toolset[0] && strcmp(active_toolset, "outer-toolset"));
   memset(out, 0, sizeof(*out));
   out->success = 1;
   if (user_prompt && strstr(user_prompt, "secret"))
      out->response = strdup("api_key = sk-testtrajectorysecret");
   else
      out->response = strdup(user_prompt && strstr(user_prompt, "second") ? "second done" : "done");
   snprintf(out->agent_name, sizeof(out->agent_name), "unit-agent");
   snprintf(out->canonical_session_id, sizeof out->canonical_session_id,
            "trajectory-fixture-%ld-%d", (long)getpid(), ++serial);
   const char *owner = agent_get_request_vault_principal();
   assert(!db1_server_session_create(out->canonical_session_id, "delegate", owner));
   aimee_block_t input = {.type = AIMEE_BLK_TEXT, .text = (char *)user_prompt};
   aimee_block_t answer = {.type = AIMEE_BLK_TEXT, .text = out->response};
   aimee_message_t user = {.role = "user", .blocks = &input, .n_blocks = 1};
   aimee_message_t model = {.role = "assistant", .blocks = &answer, .n_blocks = 1};
   aimee_block_t opaque[] = {answer,
                             {.type = AIMEE_BLK_TOOL_USE,
                              .tool_id = "opaque-call",
                              .tool_name = "Read",
                              .tool_input = NULL}};
   if (strstr(user_prompt, "opaque args"))
   {
      opaque[1].tool_input =
          cJSON_Parse(strstr(user_prompt, "nested") ? "{\"password\":{\"value\":\"ordinary\"}}"
                                                    : "{\"api_key\":\"ordinary\"}");
      assert(opaque[1].tool_input);
      model.blocks = opaque;
      model.n_blocks = 2;
   }
   aimee_response_t response = {
       .role = "assistant", .content = &answer, .n_content = 1, .stop_reason = AIMEE_STOP_END_TURN};
   assert(!aimee_ir_response_clone(&response, &out->canonical_response));
   cJSON *history = cJSON_CreateArray();
   assert(history && cJSON_AddItemToArray(history, aimee_ir_message_json(&user)) &&
          cJSON_AddItemToArray(history, aimee_ir_message_json(&model)));
   char *wire = cJSON_PrintUnformatted(history);
   assert(wire && !db1_session_ir_history_save(out->canonical_session_id, "checkpoint", "primary",
                                               owner, wire));
   free(wire);
   cJSON_Delete(history);
   cJSON_Delete(opaque[1].tool_input);
   agent_result_replace_display_text(out, strdup("display must not determine task grading"));
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
   char out_dir[256];
   snprintf(out_dir, sizeof(out_dir), "%s/aimee-traj-batch-%ld", platform_tmpdir(), (long)getpid());
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
   agent_tools_set_active_toolset("outer-toolset");
   setenv("AIMEE_ACTIVE_TOOLSET", "outer-env", 1);
   assert(trajectory_batch_run(&cfg, &opts, &summary) == 0);
   assert(!strcmp(active_toolset, "outer-toolset"));
   assert(!strcmp(getenv("AIMEE_ACTIVE_TOOLSET"), "outer-env"));
   unsetenv("AIMEE_ACTIVE_TOOLSET");
   cJSON *root = cJSON_Parse(summary);
   assert(root != NULL);
   assert(cJSON_GetObjectItem(root, "tasks")->valueint == 2);
   cJSON *written = cJSON_GetObjectItem(root, "written");
   assert(cJSON_IsArray(written));
   assert(cJSON_GetArraySize(written) == 2);
   for (int i = 0; i < 2; i++)
      assert(cJSON_IsTrue(
          cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(written, i), "success")));
   cJSON *counts = cJSON_GetObjectItem(root, "toolsets");
   assert(cJSON_GetObjectItem(counts, "readonly")->valueint == 1);
   assert(cJSON_GetObjectItem(counts, "current_code")->valueint == 1);
   cJSON *first = cJSON_GetArrayItem(written, 0);
   cJSON *second = cJSON_GetArrayItem(written, 1);
   assert(access(cJSON_GetObjectItem(first, "path")->valuestring, F_OK) == 0);
   assert(access(cJSON_GetObjectItem(second, "path")->valuestring, F_OK) == 0);
   for (int i = 0; i < 2; i++)
   {
      cJSON *item = cJSON_GetArrayItem(written, i);
      const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "path"));
      FILE *file = fopen(path, "r");
      assert(file);
      char buffer[8192];
      size_t length = fread(buffer, 1, sizeof buffer - 1, file);
      fclose(file);
      buffer[length] = 0;
      cJSON *artifact = cJSON_Parse(buffer);
      assert(artifact &&
             !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(artifact, "schema")),
                     "aimee.trajectory.v2"));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(artifact, "session_id")),
                     cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "session_id"))));
      assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(artifact, "messages")) == 2);
      assert(!cJSON_GetObjectItemCaseSensitive(artifact, "outcome"));
      cJSON_Delete(artifact);
   }
   cJSON_Delete(root);
   free(summary);
   /* Missing turn identity refuses before executing a task or writing output. */
   attested_session = "";
   summary = (char *)1;
   assert(trajectory_batch_run(&cfg, &opts, &summary) == -1 && summary == NULL);
   attested_session = "fixture-parent";

   platform_test_remove_sqlite(db_path);
   platform_test_rmrf(out_dir);
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
   char out_dir[256];
   snprintf(out_dir, sizeof(out_dir), "%s/aimee-traj-batch-secret-%ld", platform_tmpdir(),
            (long)getpid());
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

   platform_test_remove_sqlite(db_path);
   platform_test_rmrf(out_dir);
   printf("  PASS: test_batch_refuses_unresolved_secret_without_file\n");
}

static void test_batch_refuses_contextual_opaque_credentials(void)
{
   char root[256];
   snprintf(root, sizeof root, "%s/trajectory-opaque-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root));
   char tasks[512];
   snprintf(tasks, sizeof tasks, "%s/tasks.jsonl", root);
   write_text(tasks,
              "{\"name\":\"opaque\",\"prompt\":\"opaque args\",\"role\":\"execute\"}\n"
              "{\"name\":\"nested\",\"prompt\":\"opaque args nested\",\"role\":\"execute\"}\n");
   agent_config_t cfg = {0};
   trajectory_batch_opts_t opts = {
       .tasks_path = tasks, .out_dir = root, .export_opts = {.redact = 1}};
   char *summary = NULL;
   assert(trajectory_batch_run(&cfg, &opts, &summary) == -1 && summary);
   cJSON *result = cJSON_Parse(summary);
   assert(result && cJSON_GetObjectItemCaseSensitive(result, "failures")->valueint == 2);
   assert(!cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(result, "written")));
   char output[512];
   snprintf(output, sizeof output, "%s/001-opaque.jsonl", root);
   assert(access(output, F_OK));
   cJSON_Delete(result);
   free(summary);
   platform_test_rmrf(root);
   puts("  PASS: contextual opaque credential refuses export through real Go screening");
}

int main(void)
{
   /* The store is a module now. Without one attached every db1_* call below
      fails, so bring the real one up -- or skip, saying why, on a machine with
      no database to point it at. */
   if (!store_module_fixture_available())
      return 0;
   store_module_fixture_start();

   printf("trajectory_batch:\n");
   test_batch_writes_one_trajectory_per_task();
   test_batch_refuses_unresolved_secret_without_file();
   test_batch_refuses_contextual_opaque_credentials();
   printf("trajectory_batch: all tests passed\n");
   return 0;
}
