/* test_ensemble.c: unit tests for templated multi-agent ensembles */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "db1.h"
#include "platform_test_util.h"

static void open_test_db(const char *path)
{
   assert(db1_init(path) == 0);
}

static void close_test_db(void)
{
   db1_shutdown();
}

static void write_template(const char *root)
{
   char dir[512];
   snprintf(dir, sizeof(dir), "%s/ensemble_templates", root);
   assert(mkdir(dir, 0700) == 0);

   char path[512];
   snprintf(path, sizeof(path), "%s/code-review.json", dir);
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   fputs("{"
         "\"name\":\"code-review\","
         "\"phases\":["
         "{\"name\":\"initial-review\",\"participants\":[{\"role\":\"reviewer\"},{\"role\":"
         "\"reviewer\"}]},"
         "{\"name\":\"rebuttal\",\"participants\":[{\"role\":\"author\"}]},"
         "{\"name\":\"final-verdict\",\"participants\":[{\"role\":\"reviewer\"},{\"role\":"
         "\"reviewer\"}]}"
         "]}",
         fp);
   fclose(fp);
}

static cJSON *make_assignments(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON *reviewer = cJSON_AddArrayToObject(root, "reviewer");
   cJSON_AddItemToArray(reviewer, cJSON_CreateString("claude-1"));
   cJSON_AddItemToArray(reviewer, cJSON_CreateString("gemini"));
   cJSON *author = cJSON_AddArrayToObject(root, "author");
   cJSON_AddItemToArray(author, cJSON_CreateString("claude-2"));
   return root;
}

static void test_create_and_progress(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-ensemble-dir-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);
   write_template(tmpdir);

   char tmpdb[512];
   snprintf(tmpdb, sizeof(tmpdb), "%s/aimee-test-ensemble-db-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmpdb, sizeof(tmpdb), "aim");
   assert(fd >= 0);
   close(fd);

   open_test_db(tmpdb);
   cJSON *assignments = make_assignments();
   int id = 0;
   char err[256] = "";
   assert(db1_ensemble_create(tmpdir, "code-review", "review", assignments, &id, err,
                              sizeof(err)) == 0);
   cJSON_Delete(assignments);

   ensemble_info_t info;
   char *prompt = NULL;
   char *context = NULL;
   assert(db1_ensemble_get(id, &info, &prompt, &context, err, sizeof(err)) == 0);
   assert(strcmp(info.expected_agent, "claude-1") == 0);
   assert(strcmp(info.expected_role, "reviewer") == 0);
   assert(strstr(prompt, "Provide your own independent analysis") != NULL);
   free(prompt);
   free(context);

   assert(db1_ensemble_advance(id, "human", "Please check auth flows first", &info, &prompt, err,
                               sizeof(err)) == 0);
   assert(strcmp(info.status, "paused") == 0);
   assert(strstr(prompt, "Recent context") != NULL);
   free(prompt);

   assert(db1_ensemble_advance(id, "claude-1", "I found one correctness issue", &info, &prompt, err,
                               sizeof(err)) == 0);
   assert(strcmp(info.status, "active") == 0);
   assert(strcmp(info.expected_agent, "gemini") == 0);
   free(prompt);

   assert(db1_ensemble_advance(id, "gemini", "I found a second issue", &info, &prompt, err,
                               sizeof(err)) == 0);
   assert(strcmp(info.expected_agent, "claude-2") == 0);
   assert(strstr(prompt, "independent analysis") == NULL);
   free(prompt);

   assert(db1_ensemble_advance(id, "claude-2", "I will fix both", &info, &prompt, err,
                               sizeof(err)) == 0);
   assert(strcmp(info.expected_agent, "claude-1") == 0);
   free(prompt);

   assert(db1_ensemble_advance(id, "claude-1", "Looks fixed", &info, &prompt, err, sizeof(err)) ==
          0);
   free(prompt);
   assert(db1_ensemble_advance(id, "gemini", "Agreed", &info, &prompt, err, sizeof(err)) == 0);
   assert(strcmp(info.status, "complete") == 0);
   assert(prompt != NULL && strcmp(prompt, "") == 0);
   free(prompt);

   close_test_db();
   platform_test_remove_sqlite(tmpdb);
   platform_test_rmrf(tmpdir);
}

static void test_wrong_agent_rejected(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-ensemble-dir-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);
   write_template(tmpdir);

   char tmpdb[512];
   snprintf(tmpdb, sizeof(tmpdb), "%s/aimee-test-ensemble-db-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmpdb, sizeof(tmpdb), "aim");
   assert(fd >= 0);
   close(fd);

   open_test_db(tmpdb);
   cJSON *assignments = make_assignments();
   int id = 0;
   char err[256] = "";
   assert(db1_ensemble_create(tmpdir, "code-review", "review", assignments, &id, err,
                              sizeof(err)) == 0);
   cJSON_Delete(assignments);

   ensemble_info_t info;
   char *prompt = NULL;
   assert(db1_ensemble_advance(id, "gemini", "out of order", &info, &prompt, err, sizeof(err)) ==
          -1);
   assert(strstr(err, "expected 'claude-1'") != NULL);
   free(prompt);

   close_test_db();
   platform_test_remove_sqlite(tmpdb);
   platform_test_rmrf(tmpdir);
}

static void test_list_and_json(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-ensemble-dir-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);
   write_template(tmpdir);

   char tmpdb[512];
   snprintf(tmpdb, sizeof(tmpdb), "%s/aimee-test-ensemble-db-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmpdb, sizeof(tmpdb), "aim");
   assert(fd >= 0);
   close(fd);

   open_test_db(tmpdb);

   /* Empty list */
   ensemble_info_t *rows = NULL;
   int n = 0;
   char err[256] = "";
   assert(db1_ensemble_list(&rows, &n, err, sizeof(err)) == 0);
   assert(n == 0);
   free(rows);

   /* Create two sessions */
   cJSON *a1 = make_assignments();
   int id1 = 0;
   assert(db1_ensemble_create(tmpdir, "code-review", "review-a", a1, &id1, err, sizeof(err)) == 0);
   cJSON_Delete(a1);

   cJSON *a2 = make_assignments();
   int id2 = 0;
   assert(db1_ensemble_create(tmpdir, "code-review", "review-b", a2, &id2, err, sizeof(err)) == 0);
   cJSON_Delete(a2);

   /* Pause the second one and check list sees both with correct status */
   assert(db1_ensemble_pause(id2, "manual", err, sizeof(err)) == 0);

   rows = NULL;
   n = 0;
   assert(db1_ensemble_list(&rows, &n, err, sizeof(err)) == 0);
   assert(n == 2);
   assert(rows[0].id == id1);
   assert(strcmp(rows[0].status, "active") == 0);
   assert(strcmp(rows[0].channel, "review-a") == 0);
   assert(rows[0].phase_count == 3);
   assert(strcmp(rows[0].phase_name, "initial-review") == 0);
   assert(rows[1].id == id2);
   assert(strcmp(rows[1].status, "paused") == 0);
   assert(strcmp(rows[1].paused_reason, "manual") == 0);

   cJSON *js = db1_ensemble_info_to_json(&rows[0], "prompt-text", "ctx-text");
   assert(js != NULL);
   cJSON *jtmpl = cJSON_GetObjectItemCaseSensitive(js, "template");
   cJSON *jstatus = cJSON_GetObjectItemCaseSensitive(js, "status");
   cJSON *jphase = cJSON_GetObjectItemCaseSensitive(js, "phase_name");
   cJSON *jprompt = cJSON_GetObjectItemCaseSensitive(js, "next_prompt");
   cJSON *jctx = cJSON_GetObjectItemCaseSensitive(js, "recent_context");
   cJSON *jphase_count = cJSON_GetObjectItemCaseSensitive(js, "phase_count");
   assert(cJSON_IsString(jtmpl) && strcmp(jtmpl->valuestring, "code-review") == 0);
   assert(cJSON_IsString(jstatus) && strcmp(jstatus->valuestring, "active") == 0);
   assert(cJSON_IsString(jphase) && strcmp(jphase->valuestring, "initial-review") == 0);
   assert(cJSON_IsString(jprompt) && strcmp(jprompt->valuestring, "prompt-text") == 0);
   assert(cJSON_IsString(jctx) && strcmp(jctx->valuestring, "ctx-text") == 0);
   assert(cJSON_IsNumber(jphase_count) && jphase_count->valueint == 3);
   cJSON_Delete(js);

   /* Omitting prompt/context leaves those fields out */
   js = db1_ensemble_info_to_json(&rows[1], NULL, NULL);
   assert(cJSON_GetObjectItemCaseSensitive(js, "next_prompt") == NULL);
   assert(cJSON_GetObjectItemCaseSensitive(js, "recent_context") == NULL);
   cJSON_Delete(js);

   free(rows);
   close_test_db();
   platform_test_remove_sqlite(tmpdb);
   platform_test_rmrf(tmpdir);
}

static void test_current_by_channel_prefers_active(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-ensemble-dir-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);
   write_template(tmpdir);

   char tmpdb[512];
   snprintf(tmpdb, sizeof(tmpdb), "%s/aimee-test-ensemble-db-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmpdb, sizeof(tmpdb), "aim");
   assert(fd >= 0);
   close(fd);

   open_test_db(tmpdb);
   char err[256] = "";

   cJSON *first = make_assignments();
   int paused_id = 0;
   assert(db1_ensemble_create(tmpdir, "code-review", "shared", first, &paused_id, err,
                              sizeof(err)) == 0);
   cJSON_Delete(first);
   assert(db1_ensemble_pause(paused_id, "manual", err, sizeof(err)) == 0);

   cJSON *second = make_assignments();
   int active_id = 0;
   assert(db1_ensemble_create(tmpdir, "code-review", "shared", second, &active_id, err,
                              sizeof(err)) == 0);
   cJSON_Delete(second);

   ensemble_info_t info;
   char *prompt = NULL;
   char *context = NULL;
   int current_id = 0;
   assert(db1_ensemble_find_current_by_channel("shared", &current_id, err, sizeof(err)) == 0);
   assert(db1_ensemble_get(current_id, &info, &prompt, &context, err, sizeof(err)) == 0);
   assert(info.id == active_id);
   assert(strcmp(info.status, "active") == 0);
   assert(strcmp(info.channel, "shared") == 0);
   assert(prompt != NULL && strstr(prompt, "Provide your own independent analysis") != NULL);
   free(prompt);
   free(context);

   prompt = NULL;
   context = NULL;
   assert(db1_ensemble_find_current_by_channel("missing", &current_id, err, sizeof(err)) == -1);
   assert(strstr(err, "not found") != NULL);
   free(prompt);
   free(context);

   close_test_db();
   platform_test_remove_sqlite(tmpdb);
   platform_test_rmrf(tmpdir);
}

int main(void)
{
   test_create_and_progress();
   test_wrong_agent_rejected();
   test_list_and_json();
   test_current_by_channel_prefers_active();
   printf("test_ensemble: all tests passed\n");
   return 0;
}
