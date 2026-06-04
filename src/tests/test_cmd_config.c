#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "cJSON.h"
#include "commands.h"
#include "config_fields.h"
#include "platform_path.h"
#include "platform_test_util.h"

static void restore_env(const char *name, char *old_value)
{
   if (old_value)
      assert(platform_setenv(name, old_value) == 0);
   else
      assert(platform_unsetenv(name) == 0);
   free(old_value);
}

static char *capture_stdout(void (*fn)(void *), void *arg)
{
   int pipefd[2];
   assert(pipe(pipefd) == 0);

   fflush(stdout);
   int saved_stdout = dup(STDOUT_FILENO);
   assert(saved_stdout >= 0);
   assert(dup2(pipefd[1], STDOUT_FILENO) >= 0);
   close(pipefd[1]);

   fn(arg);

   fflush(stdout);
   assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
   close(saved_stdout);

   char buf[4096];
   ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
   assert(n >= 0);
   close(pipefd[0]);
   buf[n] = '\0';
   return strdup(buf);
}

typedef struct
{
   int argc;
   char **argv;
} cmd_call_t;

static void run_cmd_config(void *arg)
{
   cmd_call_t *call = (cmd_call_t *)arg;
   cmd_config(NULL, call->argc, call->argv);
}

static void write_text(const char *path, const char *content)
{
   FILE *fp = fopen(path, "w");
   assert(fp);
   fputs(content, fp);
   fclose(fp);
}

static void test_dispositions_text_and_json(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-cmd-config-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_dir[640];
   snprintf(config_dir, sizeof(config_dir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(config_dir, 0700) == 0);

   char config_path[768];
   snprintf(config_path, sizeof(config_path), "%s/aimee.yaml", config_dir);
   write_text(config_path, "memory:\n"
                           "  dispositions:\n"
                           "    global:\n"
                           "      skepticism: 0.8\n"
                           "    workspace:\n"
                           "      empathy: 0.6\n"
                           "    project:\n"
                           "      skepticism: 0.2\n");

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   char *old_aimee_home = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
   char *old_aimee_profile = getenv("AIMEE_PROFILE") ? strdup(getenv("AIMEE_PROFILE")) : NULL;
   char *old_no_cache = getenv("AIMEE_NO_CACHE") ? strdup(getenv("AIMEE_NO_CACHE")) : NULL;
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_unsetenv("AIMEE_HOME") == 0);
   assert(platform_unsetenv("AIMEE_PROFILE") == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   char *argv_text[] = {"dispositions"};
   cmd_call_t text_call = {.argc = 1, .argv = argv_text};
   char *text = capture_stdout(run_cmd_config, &text_call);
   assert(text);
   assert(strstr(text, "skepticism\t0.20\tproject") != NULL);
   assert(strstr(text, "empathy\t0.60\tworkspace") != NULL);
   free(text);

   char *argv_json[] = {"dispositions", "--json"};
   cmd_call_t json_call = {.argc = 2, .argv = argv_json};
   char *json = capture_stdout(run_cmd_config, &json_call);
   assert(json);
   assert(strstr(json, "\"name\":\"skepticism\"") != NULL);
   assert(strstr(json, "\"source\":\"project\"") != NULL);
   assert(strstr(json, "\"source\":\"workspace\"") != NULL);
   free(json);

   restore_env("HOME", old_home);
   restore_env("AIMEE_HOME", old_aimee_home);
   restore_env("AIMEE_PROFILE", old_aimee_profile);
   restore_env("AIMEE_NO_CACHE", old_no_cache);
   platform_test_rmrf(tmpdir);
}

/* The config.show/get/set server handlers and the `aimee config` command both
 * resolve fields through this shared table; exercise the pure helpers here. */
static void test_config_fields_helpers(void)
{
   assert(config_field_lookup("provider") != NULL);
   assert(config_field_lookup("autonomous") != NULL);
   assert(config_field_lookup("no_such_key") == NULL);

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));

   /* string */
   const config_field_t *provider = config_field_lookup("provider");
   assert(config_field_set_value(&cfg, provider, "claude") == 0);
   cJSON *v = config_field_value_json(&cfg, provider);
   assert(cJSON_IsString(v) && strcmp(v->valuestring, "claude") == 0);
   cJSON_Delete(v);

   /* bool: accepts true/1/false/0, rejects anything else */
   const config_field_t *auton = config_field_lookup("autonomous");
   assert(config_field_set_value(&cfg, auton, "true") == 0);
   v = config_field_value_json(&cfg, auton);
   assert(cJSON_IsBool(v) && cJSON_IsTrue(v));
   cJSON_Delete(v);
   assert(config_field_set_value(&cfg, auton, "0") == 0);
   v = config_field_value_json(&cfg, auton);
   assert(cJSON_IsBool(v) && !cJSON_IsTrue(v));
   cJSON_Delete(v);
   assert(config_field_set_value(&cfg, auton, "maybe") == -1);

   /* int */
   const config_field_t *iters = config_field_lookup("max_iterations");
   assert(iters && config_field_set_value(&cfg, iters, "42") == 0);
   v = config_field_value_json(&cfg, iters);
   assert(cJSON_IsNumber(v) && v->valueint == 42);
   cJSON_Delete(v);

   /* float — including a threshold field that used to be mistyped CFG_STRING */
   const config_field_t *thr = config_field_lookup("guardrails_semantic_warn_threshold");
   assert(thr && config_field_set_value(&cfg, thr, "0.25") == 0);
   v = config_field_value_json(&cfg, thr);
   assert(cJSON_IsNumber(v) && v->valuedouble > 0.24 && v->valuedouble < 0.26);
   cJSON_Delete(v);
}

int main(void)
{
   printf("cmd_config: ");
   test_dispositions_text_and_json();
   test_config_fields_helpers();
   printf("OK\n");
   return 0;
}
