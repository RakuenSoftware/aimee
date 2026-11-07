/* test_events.c: unit tests for event notification hooks */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aimee.h"
#include "events.h"
#include "platform_path.h"
#include "platform_test_util.h"

typedef struct
{
   char tmpdir[512];
   char *old_home;
   char *old_aimee_home;
   char *old_aimee_profile;
} events_test_env_t;

static void events_restore_env(const char *name, char *old_value)
{
   if (old_value)
      assert(platform_setenv(name, old_value) == 0);
   else
      assert(platform_unsetenv(name) == 0);
   free(old_value);
}

static void events_test_setup(events_test_env_t *env, const char *prefix)
{
   memset(env, 0, sizeof(*env));
   snprintf(env->tmpdir, sizeof(env->tmpdir), "%s/%s-XXXXXX", platform_tmpdir(), prefix);
   assert(platform_mkdtemp(env->tmpdir) != NULL);

   env->old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   env->old_aimee_home = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
   env->old_aimee_profile = getenv("AIMEE_PROFILE") ? strdup(getenv("AIMEE_PROFILE")) : NULL;

   assert(platform_setenv("HOME", env->tmpdir) == 0);
   assert(platform_unsetenv("AIMEE_HOME") == 0);
   assert(platform_unsetenv("AIMEE_PROFILE") == 0);
}

static void events_test_teardown(events_test_env_t *env)
{
   events_restore_env("HOME", env->old_home);
   events_restore_env("AIMEE_HOME", env->old_aimee_home);
   events_restore_env("AIMEE_PROFILE", env->old_aimee_profile);
   platform_test_rmrf(env->tmpdir);
}

static void test_notify_build_ntfy_command(void)
{
   notify_target_t target;
   char cmd[1024];

   printf("  test_notify_build_ntfy_command...");
   memset(&target, 0, sizeof(target));
   target.is_delivery = 1;
   assert(delivery_target_parse("ntfy:homelab-alerts", &target.target) == 0);
   snprintf(target.base_url, sizeof(target.base_url), "https://ntfy.example/root/");

   assert(notify_build_ntfy_command(&target, "verify_fail", "build failed", cmd, sizeof(cmd)) == 0);
   assert(strstr(cmd, "curl -s -X POST") != NULL);
   assert(strstr(cmd, "-H 'Title: aimee: verify_fail'") != NULL);
   assert(strstr(cmd, "-H 'Priority: default'") != NULL);
   assert(strstr(cmd, "-H 'Tags: aimee'") != NULL);
   assert(strstr(cmd, "-d 'build failed'") != NULL);
   assert(strstr(cmd, "'https://ntfy.example/root/homelab-alerts'") != NULL);

   memset(target.base_url, 0, sizeof(target.base_url));
   assert(notify_build_ntfy_command(&target, NULL, "fallback base", cmd, sizeof(cmd)) == 0);
   assert(strstr(cmd, "-H 'Title: aimee: event'") != NULL);
   assert(strstr(cmd, "'https://ntfy.sh/homelab-alerts'") != NULL);
   assert(notify_build_ntfy_command(&target, "it'ready", "can't build", cmd, sizeof(cmd)) == 0);
   assert(strstr(cmd, "it'\\''ready") != NULL);
   assert(strstr(cmd, "can'\\''t build") != NULL);
   assert(notify_build_ntfy_command(&target, "verify_fail", "msg", NULL, sizeof(cmd)) == -1);
   assert(notify_build_ntfy_command(&target, "verify_fail", "msg", cmd, 0) == -1);
   target.is_delivery = 0;
   assert(notify_build_ntfy_command(&target, "verify_fail", "msg", cmd, sizeof(cmd)) == -1);

   memset(&target, 0, sizeof(target));
   target.is_delivery = 1;
   assert(delivery_target_parse("ntfy:bad/topic", &target.target) == 0);
   assert(notify_build_ntfy_command(&target, "verify_fail", "msg", cmd, sizeof(cmd)) == -1);
   printf("ok\n");
}

static void test_notify_deliver_target_local_writes_jsonl(void)
{
   events_test_env_t env;
   notify_target_t target;
   char notify_path[MAX_PATH_LEN];

   printf("  test_notify_deliver_target_local_writes_jsonl...");
   events_test_setup(&env, "aimee-test-events-direct-local-deliver");

   memset(&target, 0, sizeof(target));
   target.is_delivery = 1;
   assert(delivery_target_parse("local", &target.target) == 0);
   assert(notify_deliver_target(&target, "cron:pulse", "pulse OK") == 0);

   snprintf(notify_path, sizeof(notify_path), "%s/.cache/aimee/notifications/events.jsonl",
            env.tmpdir);
   FILE *fp = fopen(notify_path, "r");
   assert(fp != NULL);
   char buf[1024];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';
   assert(strstr(buf, "\"event\":\"cron:pulse\"") != NULL);
   assert(strstr(buf, "\"message\":\"pulse OK\"") != NULL);

   events_test_teardown(&env);
   printf("ok\n");
}

int main(void)
{
   test_notify_build_ntfy_command();
   test_notify_deliver_target_local_writes_jsonl();

   printf("All event notification tests passed.\n");
   return 0;
}
