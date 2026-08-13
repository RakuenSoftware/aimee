/* test_trajectory.c: trajectory export and compression tests. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "db1.h"
#include "interaction_events.h"
#include "platform_test_util.h"
#include "trajectory.h"

static void test_export_normalizes_steps_and_outcome(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);
   assert(ie_record("traj-1", IE_USER_TURN, NULL, "{\"content\":\"build it\"}", "ok") == 0);
   assert(ie_record("traj-1", IE_TOOL_CALL, NULL, "{\"name\":\"bash\",\"args\":{\"cmd\":\"make\"}}",
                    "ok") == 0);
   assert(ie_record("traj-1", IE_TOOL_OUTCOME, NULL, "{\"name\":\"bash\",\"result\":\"pass\"}",
                    "ok") == 0);
   assert(ie_record("traj-1", IE_AGENT_TURN, NULL, "{\"content\":\"done\"}", "ok") == 0);

   trajectory_opts_t opts = {.compress = 1, .redact = 1, .max_tool_result_bytes = 64};
   char *json = NULL;
   assert(trajectory_export("traj-1", &opts, &json) == 0);
   assert(json != NULL);
   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);
   assert(strcmp(cJSON_GetObjectItem(root, "schema")->valuestring, "aimee.trajectory.v1") == 0);
   cJSON *steps = cJSON_GetObjectItem(root, "steps");
   assert(cJSON_IsArray(steps));
   assert(cJSON_GetArraySize(steps) == 4);
   cJSON *tool = cJSON_GetArrayItem(steps, 2);
   assert(strcmp(cJSON_GetObjectItem(tool, "role")->valuestring, "tool") == 0);
   assert(strcmp(cJSON_GetObjectItem(tool, "result")->valuestring, "pass") == 0);
   cJSON *outcome = cJSON_GetObjectItem(root, "outcome");
   assert(strcmp(cJSON_GetObjectItem(outcome, "status")->valuestring, "success") == 0);
   cJSON_Delete(root);
   free(json);
   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_compresses_large_tool_result(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);
   assert(ie_record("traj-2", IE_TOOL_OUTCOME, NULL,
                    "{\"name\":\"read_file\",\"result\":\"abcdefghijklmnopqrstuvwxyz\"}",
                    "ok") == 0);

   trajectory_opts_t opts = {.compress = 1, .redact = 1, .max_tool_result_bytes = 8};
   char *json = NULL;
   assert(trajectory_export("traj-2", &opts, &json) == 0);
   cJSON *root = cJSON_Parse(json);
   cJSON *step = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "steps"), 0);
   assert(strcmp(cJSON_GetObjectItem(step, "result")->valuestring, "[compressed tool result]") ==
          0);
   assert(strstr(cJSON_GetObjectItem(step, "result_ref")->valuestring, "hash=fnv1a64:") != NULL);
   cJSON_Delete(root);
   free(json);
   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_redacts_exported_secret(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);
   assert(ie_record("traj-3", IE_USER_TURN, NULL,
                    "{\"content\":\"api_key = sk-testtrajectorysecret\"}", "ok") == 0);

   trajectory_opts_t opts = {.compress = 1, .redact = 1, .max_tool_result_bytes = 64};
   char *json = NULL;
   assert(trajectory_export("traj-3", &opts, &json) == 0);
   assert(strstr(json, "sk-testtrajectorysecret") == NULL);
   assert(strstr(json, "[REDACTED]") != NULL);
   free(json);
   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_refuses_unresolved_secret(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);
   assert(ie_record("traj-4", IE_USER_TURN, NULL,
                    "{\"content\":\"api_key = sk-testtrajectorysecret\"}", "ok") == 0);

   trajectory_opts_t opts = {
       .compress = 1, .redact = 1, .max_tool_result_bytes = 64, .redaction_buf_bytes = 8};
   char *json = NULL;
   assert(trajectory_export("traj-4", &opts, &json) != 0);
   assert(json == NULL);
   db1_shutdown();
   platform_test_remove_sqlite(path);
}

int main(void)
{
   char path[256];
   snprintf(path, sizeof path, "%s/test_trajectory_XXXXXX.db", platform_tmpdir());
   int fd = mkstemps(path, 3);
   if (fd >= 0)
      close(fd);

   test_export_normalizes_steps_and_outcome(path);
   test_compresses_large_tool_result(path);
   test_redacts_exported_secret(path);
   test_refuses_unresolved_secret(path);
   printf("trajectory: all tests passed\n");
   return 0;
}
