/* test_cmd_onboard.c: coverage for the guided onboard orchestrator.
 * Exercises the pure report builder so we don't run the full
 * filesystem-touching cmd_setup in a unit test. */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"
#include "cJSON.h"
#include "commands.h"
#include "db1_client/db1.h"
#include "memory.h"
#include "platform_test_util.h"

typedef int (*onboard_memory_insert_fn)(const char *tier, const char *kind, const char *key,
                                        const char *content, double confidence,
                                        const char *session_id, memory_t *out);
typedef int (*onboard_memory_get_fn)(int64_t id, memory_t *out);
void onboard_set_memory_client_for_test(onboard_memory_insert_fn insert_fn,
                                        onboard_memory_get_fn get_fn);

static memory_t g_onboard_memory_smoke;
static int64_t g_onboard_memory_next_id = 1000;

static int test_onboard_memory_insert(const char *tier, const char *kind, const char *key,
                                      const char *content, double confidence,
                                      const char *session_id, memory_t *out)
{
   memset(&g_onboard_memory_smoke, 0, sizeof(g_onboard_memory_smoke));
   g_onboard_memory_smoke.id = g_onboard_memory_next_id++;
   snprintf(g_onboard_memory_smoke.tier, sizeof(g_onboard_memory_smoke.tier), "%s",
            tier ? tier : "");
   snprintf(g_onboard_memory_smoke.kind, sizeof(g_onboard_memory_smoke.kind), "%s",
            kind ? kind : "");
   snprintf(g_onboard_memory_smoke.key, sizeof(g_onboard_memory_smoke.key), "%s", key ? key : "");
   snprintf(g_onboard_memory_smoke.content, sizeof(g_onboard_memory_smoke.content), "%s",
            content ? content : "");
   g_onboard_memory_smoke.confidence = confidence;
   snprintf(g_onboard_memory_smoke.source_session, sizeof(g_onboard_memory_smoke.source_session),
            "%s", session_id ? session_id : "");
   if (out)
      *out = g_onboard_memory_smoke;
   return 0;
}

static int test_onboard_memory_get(int64_t id, memory_t *out)
{
   if (id != g_onboard_memory_smoke.id || !out)
      return -1;
   *out = g_onboard_memory_smoke;
   return 0;
}

static void scratch_env(char *tmpdir, size_t cap)
{
   snprintf(tmpdir, cap, "%s/aimee-test-onboard-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_dir[4096];
   snprintf(config_dir, sizeof(config_dir), "%s/.config/aimee", tmpdir);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", config_dir);
   assert(system(cmd) == 0);

   platform_setenv("HOME", tmpdir);
   platform_setenv("AIMEE_NO_CACHE", "1");

   config_t cfg;
   config_load(&cfg);
   assert(config_save(&cfg) == 0);
}

static void scratch_cleanup(const char *tmpdir)
{
   char cmd[640];
   /* The onboarding flow under test autostarts an aimee-kb daemon bound to a
    * socket under tmpdir. Kill it before removing the home so the daemon is not
    * orphaned: leaked aimee-kb daemons retry DB2 provisioning and were the root
    * source of the stuck-`createdb` runaway (see #2569). tmpdir is a unique
    * mkdtemp path, so this only targets this test's own daemon. */
   snprintf(cmd, sizeof(cmd), "pkill -KILL -f 'aimee-kb --socket=%s' 2>/dev/null", tmpdir);
   (void)system(cmd);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   (void)system(cmd);
   platform_unsetenv("AIMEE_NO_CACHE");
}

static void test_onboard_report_shape_with_skip_setup(void)
{
   char tmpdir[512];
   scratch_env(tmpdir, sizeof(tmpdir));

   /* Silence stdout/stderr while the report runs; the inner doctor
    * path prints to stderr and we don't want that in the test run. */
   fflush(stdout);
   fflush(stderr);
   int saved_stdout = dup(STDOUT_FILENO);
   int saved_stderr = dup(STDERR_FILENO);
   int dev_null = open("/dev/null", O_WRONLY);
   assert(dev_null >= 0);
   dup2(dev_null, STDOUT_FILENO);
   dup2(dev_null, STDERR_FILENO);
   close(dev_null);

   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.json_output = 1;

   cJSON *report = onboard_build_report(&ctx, /* skip_setup */ 1);

   dup2(saved_stdout, STDOUT_FILENO);
   close(saved_stdout);
   dup2(saved_stderr, STDERR_FILENO);
   close(saved_stderr);

   assert(report != NULL);

   cJSON *version = cJSON_GetObjectItemCaseSensitive(report, "version");
   assert(cJSON_IsString(version) && version->valuestring[0]);

   cJSON *steps = cJSON_GetObjectItemCaseSensitive(report, "steps");
   assert(cJSON_IsArray(steps));
   assert(cJSON_GetArraySize(steps) == 3);

   /* Steps in order: setup (skipped), doctor, memory_smoke. */
   cJSON *setup_step = cJSON_GetArrayItem(steps, 0);
   cJSON *doctor_step = cJSON_GetArrayItem(steps, 1);
   cJSON *smoke_step = cJSON_GetArrayItem(steps, 2);

   cJSON *setup_name = cJSON_GetObjectItemCaseSensitive(setup_step, "step");
   cJSON *setup_status = cJSON_GetObjectItemCaseSensitive(setup_step, "status");
   assert(cJSON_IsString(setup_name) && strcmp(setup_name->valuestring, "setup") == 0);
   assert(cJSON_IsString(setup_status) && strcmp(setup_status->valuestring, "skipped") == 0);

   cJSON *doctor_name = cJSON_GetObjectItemCaseSensitive(doctor_step, "step");
   cJSON *doctor_status = cJSON_GetObjectItemCaseSensitive(doctor_step, "status");
   cJSON *doctor_checks = cJSON_GetObjectItemCaseSensitive(doctor_step, "checks");
   assert(cJSON_IsString(doctor_name) && strcmp(doctor_name->valuestring, "doctor") == 0);
   assert(cJSON_IsString(doctor_status));
   /* doctor status must be one of ok/warn/error */
   assert(strcmp(doctor_status->valuestring, "ok") == 0 ||
          strcmp(doctor_status->valuestring, "warn") == 0 ||
          strcmp(doctor_status->valuestring, "error") == 0);
   assert(cJSON_IsArray(doctor_checks));

   cJSON *smoke_name = cJSON_GetObjectItemCaseSensitive(smoke_step, "step");
   cJSON *smoke_status = cJSON_GetObjectItemCaseSensitive(smoke_step, "status");
   assert(cJSON_IsString(smoke_name) && strcmp(smoke_name->valuestring, "memory_smoke") == 0);
   /* smoke must have a status (ok or error); the message field is only
    * set on failure. */
   assert(cJSON_IsString(smoke_status));

   cJSON *ready = cJSON_GetObjectItemCaseSensitive(report, "ready");
   assert(cJSON_IsBool(ready));

   cJSON *next_actions = cJSON_GetObjectItemCaseSensitive(report, "next_actions");
   assert(cJSON_IsArray(next_actions));
   /* Either the run is ready (one friendly action) or non-ready (≥1
    * remediation action); never empty. */
   assert(cJSON_GetArraySize(next_actions) >= 1);

   /* elapsed_ms is present and non-negative so the time-to-ready
    * metric has something to track from day one. Timing comparisons
    * belong in explicit benchmarks, not unit-test gates. */
   cJSON *elapsed = cJSON_GetObjectItemCaseSensitive(report, "elapsed_ms");
   assert(cJSON_IsNumber(elapsed));
   assert(elapsed->valuedouble >= 0.0);

   cJSON_Delete(report);
   scratch_cleanup(tmpdir);
}

static void test_onboard_report_is_stable_across_runs(void)
{
   /* Determinism check: running the report twice in the same
    * environment with skip_setup=1 should produce the same step
    * shape (status codes may differ if something in the env races,
    * but the step names and ordering are invariant). */
   char tmpdir[512];
   scratch_env(tmpdir, sizeof(tmpdir));

   fflush(stdout);
   fflush(stderr);
   int saved_stdout = dup(STDOUT_FILENO);
   int saved_stderr = dup(STDERR_FILENO);
   int dev_null = open("/dev/null", O_WRONLY);
   dup2(dev_null, STDOUT_FILENO);
   dup2(dev_null, STDERR_FILENO);
   close(dev_null);

   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.json_output = 1;

   cJSON *first = onboard_build_report(&ctx, 1);
   cJSON *second = onboard_build_report(&ctx, 1);

   dup2(saved_stdout, STDOUT_FILENO);
   close(saved_stdout);
   dup2(saved_stderr, STDERR_FILENO);
   close(saved_stderr);

   assert(first && second);

   cJSON *s1 = cJSON_GetObjectItemCaseSensitive(first, "steps");
   cJSON *s2 = cJSON_GetObjectItemCaseSensitive(second, "steps");
   assert(cJSON_GetArraySize(s1) == cJSON_GetArraySize(s2));
   for (int i = 0; i < cJSON_GetArraySize(s1); i++)
   {
      cJSON *n1 = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(s1, i), "step");
      cJSON *n2 = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(s2, i), "step");
      assert(cJSON_IsString(n1) && cJSON_IsString(n2));
      assert(strcmp(n1->valuestring, n2->valuestring) == 0);
   }

   cJSON_Delete(first);
   cJSON_Delete(second);
   scratch_cleanup(tmpdir);
}

int main(void)
{
   printf("cmd_onboard: ");
   onboard_set_memory_client_for_test(test_onboard_memory_insert, test_onboard_memory_get);
   test_onboard_report_shape_with_skip_setup();
   test_onboard_report_is_stable_across_runs();
   onboard_set_memory_client_for_test(NULL, NULL);
   printf("all tests passed\n");
   return 0;
}
