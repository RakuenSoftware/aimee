/* test_cmd_doctor.c: tests for the aimee doctor diagnostic command */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "db2.h"
#include "db2_test_shim.h"
#include "commands.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "cJSON.h"

typedef struct
{
   char tmpdir[512];
   char *old_home;
   char *old_aimee_home;
   char *old_aimee_profile;
   char *old_no_cache;
   char *old_kb_no_autostart;
} doctor_test_env_t;

static void restore_env(const char *name, char *old_value)
{
   if (old_value)
      assert(platform_setenv(name, old_value) == 0);
   else
      assert(platform_unsetenv(name) == 0);
   free(old_value);
}

static void doctor_test_setup(doctor_test_env_t *env, const char *prefix)
{
   memset(env, 0, sizeof(*env));
   snprintf(env->tmpdir, sizeof(env->tmpdir), "%s/%s-XXXXXX", platform_tmpdir(), prefix);
   assert(platform_mkdtemp(env->tmpdir) != NULL);

   char config_dir[4096];
   snprintf(config_dir, sizeof(config_dir), "%s/.config/aimee", env->tmpdir);
   assert(platform_mkdir_p(config_dir, 0700) == 0);

   env->old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   env->old_aimee_home = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
   env->old_aimee_profile = getenv("AIMEE_PROFILE") ? strdup(getenv("AIMEE_PROFILE")) : NULL;
   env->old_no_cache = getenv("AIMEE_NO_CACHE") ? strdup(getenv("AIMEE_NO_CACHE")) : NULL;
   const char *old_kb_env = getenv("AIMEE_KB_NO_AUTOSTART");
   env->old_kb_no_autostart = old_kb_env ? strdup(old_kb_env) : NULL;

   assert(platform_setenv("HOME", env->tmpdir) == 0);
   assert(platform_unsetenv("AIMEE_HOME") == 0);
   assert(platform_unsetenv("AIMEE_PROFILE") == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   assert(platform_setenv("AIMEE_KB_NO_AUTOSTART", "1") == 0);
}

static void doctor_test_teardown(doctor_test_env_t *env)
{
   restore_env("HOME", env->old_home);
   restore_env("AIMEE_HOME", env->old_aimee_home);
   restore_env("AIMEE_PROFILE", env->old_aimee_profile);
   restore_env("AIMEE_NO_CACHE", env->old_no_cache);
   restore_env("AIMEE_KB_NO_AUTOSTART", env->old_kb_no_autostart);
   platform_test_rmrf(env->tmpdir);
}

/* --- Test: doctor command runs without crashing on a fresh database --- */

static void test_doctor_runs_on_fresh_db(void)
{
   doctor_test_env_t env;
   doctor_test_setup(&env, "aimee-test-doctor");

   /* Initialize a database */
   config_t cfg;
   config_load(&cfg);
   assert(config_save(&cfg) == 0);
   assert(db1_init(cfg.db1_path) == 0);
   db1_shutdown();

   /* Run doctor with JSON output (to avoid exit() calls interfering) */
   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.json_output = 1;

   /* Redirect stdout to capture JSON */
   fflush(stdout);
   int saved_stdout = dup(STDOUT_FILENO);
   int dev_null = open("/dev/null", O_WRONLY);
   assert(dev_null >= 0);
   dup2(dev_null, STDOUT_FILENO);
   close(dev_null);

   /* Redirect stderr too */
   fflush(stderr);
   int saved_stderr = dup(STDERR_FILENO);
   dev_null = open("/dev/null", O_WRONLY);
   assert(dev_null >= 0);
   dup2(dev_null, STDERR_FILENO);
   close(dev_null);

   /* Doctor will call exit() on warnings/errors, so we fork */
   pid_t pid = fork();
   if (pid == 0)
   {
      /* Child: run doctor */
      cmd_doctor(&ctx, 0, NULL);
      _exit(0);
   }

   /* Parent: wait for child */
   int status;
   waitpid(pid, &status, 0);
   /* Doctor exits 0/1/2 depending on check results — all are valid */
   assert(WIFEXITED(status));
   int exit_code = WEXITSTATUS(status);
   assert(exit_code == 0 || exit_code == 1 || exit_code == 2);

   /* Restore stdout/stderr */
   dup2(saved_stdout, STDOUT_FILENO);
   close(saved_stdout);
   dup2(saved_stderr, STDERR_FILENO);
   close(saved_stderr);

   doctor_test_teardown(&env);
}

/* --- Test: doctor exits 2 on database error --- */

static void test_doctor_exits_2_on_db_error(void)
{
   doctor_test_env_t env;
   doctor_test_setup(&env, "aimee-test-doctor-err");

   /* Create config but with a DB path pointing to a non-existent location */
   config_t cfg;
   config_load(&cfg);
   /* Write config but don't create the DB */
   config_save(&cfg);

   /* Remove the DB file if it was auto-created */
   unlink(cfg.db1_path);

   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.json_output = 1;

   /* Suppress output */
   fflush(stdout);
   fflush(stderr);
   int saved_stdout = dup(STDOUT_FILENO);
   int saved_stderr = dup(STDERR_FILENO);
   int dev_null = open("/dev/null", O_WRONLY);
   dup2(dev_null, STDOUT_FILENO);
   dup2(dev_null, STDERR_FILENO);
   close(dev_null);

   pid_t pid = fork();
   if (pid == 0)
   {
      cmd_doctor(&ctx, 0, NULL);
      _exit(0);
   }

   int status;
   waitpid(pid, &status, 0);
   assert(WIFEXITED(status));
   int exit_code = WEXITSTATUS(status);
   /* Should exit 2 (errors) because DB doesn't exist */
   assert(exit_code == 2);

   dup2(saved_stdout, STDOUT_FILENO);
   close(saved_stdout);
   dup2(saved_stderr, STDERR_FILENO);
   close(saved_stderr);

   doctor_test_teardown(&env);
}

/* --- Test: doctor --fix flag is parsed --- */

static void test_doctor_fix_flag_parsed(void)
{
   doctor_test_env_t env;
   doctor_test_setup(&env, "aimee-test-doctor-fix");

   /* Initialize database */
   config_t cfg;
   config_load(&cfg);
   assert(config_save(&cfg) == 0);
   assert(db1_init(cfg.db1_path) == 0);
   db1_shutdown();

   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.json_output = 1;

   /* Suppress output */
   fflush(stdout);
   fflush(stderr);
   int saved_stdout = dup(STDOUT_FILENO);
   int saved_stderr = dup(STDERR_FILENO);
   int dev_null = open("/dev/null", O_WRONLY);
   dup2(dev_null, STDOUT_FILENO);
   dup2(dev_null, STDERR_FILENO);
   close(dev_null);

   pid_t pid = fork();
   if (pid == 0)
   {
      char *args[] = {"--fix"};
      cmd_doctor(&ctx, 1, args);
      _exit(0);
   }

   int status;
   waitpid(pid, &status, 0);
   assert(WIFEXITED(status));
   /* Should not crash with --fix flag. Any normal exit code is fine —
    * in an isolated HOME, vector-store sidecar and other auto-fixable checks may
    * legitimately remain in an error state after --fix (exit 2). */
   int exit_code = WEXITSTATUS(status);
   assert(exit_code >= 0 && exit_code <= 2);

   dup2(saved_stdout, STDOUT_FILENO);
   close(saved_stdout);
   dup2(saved_stderr, STDERR_FILENO);
   close(saved_stderr);

   doctor_test_teardown(&env);
}

static void test_doctor_reports_kb_process_when_not_running(void)
{
   doctor_test_env_t env;
   doctor_test_setup(&env, "aimee-test-doctor-kb-process");

   config_t cfg;
   config_load(&cfg);
   assert(config_save(&cfg) == 0);
   assert(db1_init(cfg.db1_path) == 0);
   db1_shutdown();

   char *json = doctor_checks_json();
   assert(json != NULL);
   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   cJSON *checks = cJSON_GetObjectItemCaseSensitive(root, "checks");
   assert(cJSON_IsArray(checks));

   cJSON *kb_process = NULL;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, checks)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
      if (cJSON_IsString(name) && strcmp(name->valuestring, "KB: process") == 0)
      {
         kb_process = item;
         break;
      }
   }

   assert(kb_process != NULL);

   cJSON *status = cJSON_GetObjectItemCaseSensitive(kb_process, "status");
   cJSON *message = cJSON_GetObjectItemCaseSensitive(kb_process, "message");
   assert(cJSON_IsString(status) && strcmp(status->valuestring, "ERROR") == 0);
   assert(cJSON_IsString(message) && strstr(message->valuestring, "aimee-kb not running") != NULL);

   cJSON_Delete(root);
   free(json);

   doctor_test_teardown(&env);
}

static void test_doctor_json_preserves_existing_db2_connection(void)
{
   doctor_test_env_t env;
   doctor_test_setup(&env, "aimee-test-doctor-db2");

   config_t cfg;
   config_load(&cfg);
   snprintf(cfg.db2_url, sizeof(cfg.db2_url), "shim");
   assert(config_save(&cfg) == 0);

   db2_test_shim_open();
   assert(db2_is_initialized());

   char *json = doctor_checks_json();
   assert(json != NULL);
   assert(db2_is_initialized());
   free(json);

   db2_test_shim_close();

   doctor_test_teardown(&env);
}

static void test_doctor_reports_guardrails_semantic_counts(void)
{
   doctor_test_env_t env;
   doctor_test_setup(&env, "aimee-test-doctor-gsem");

   config_t cfg;
   config_load(&cfg);
   snprintf(cfg.guardrails_semantic_mode, sizeof(cfg.guardrails_semantic_mode), "advisory");
   assert(config_save(&cfg) == 0);
   assert(db1_init(cfg.db1_path) == 0);

   guardrail_event_t e;
   memset(&e, 0, sizeof(e));
   snprintf(e.session_id, sizeof(e.session_id), "doctor-gsem");
   snprintf(e.tool_name, sizeof(e.tool_name), "Edit");
   snprintf(e.final_action, sizeof(e.final_action), "warn");
   e.dry_run = 0;
   assert(db1_guardrail_event_insert(&e) == 0);
   snprintf(e.final_action, sizeof(e.final_action), "prompt");
   assert(db1_guardrail_event_insert(&e) == 0);
   snprintf(e.final_action, sizeof(e.final_action), "block");
   assert(db1_guardrail_event_insert(&e) == 0);

   char *json = doctor_checks_json();
   assert(json != NULL);
   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   cJSON *checks = cJSON_GetObjectItemCaseSensitive(root, "checks");
   assert(cJSON_IsArray(checks));

   cJSON *semantic = NULL;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, checks)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
      if (cJSON_IsString(name) && strcmp(name->valuestring, "guardrails.semantic") == 0)
      {
         semantic = item;
         break;
      }
   }

   assert(semantic != NULL);
   cJSON *message = cJSON_GetObjectItemCaseSensitive(semantic, "message");
   assert(cJSON_IsString(message));
   assert(strstr(message->valuestring, "mode=advisory") != NULL);
   assert(strstr(message->valuestring, "7d warns: 1 prompts: 1 blocks: 1") != NULL);

   cJSON_Delete(root);
   free(json);
   db1_shutdown();

   doctor_test_teardown(&env);
}

int main(void)
{
   printf("cmd_doctor: ");

   test_doctor_runs_on_fresh_db();
   test_doctor_exits_2_on_db_error();
   test_doctor_fix_flag_parsed();
   test_doctor_reports_kb_process_when_not_running();
   test_doctor_json_preserves_existing_db2_connection();
   test_doctor_reports_guardrails_semantic_counts();

   printf("all tests passed\n");
   return 0;
}
