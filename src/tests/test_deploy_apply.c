/* test_deploy_apply.c — the managed-deploy command construction.
 *
 * Covers the two properties the wizard's Deploy depends on:
 *   1. the deploy never passes --remove-orphans (the managed compose shares
 *      COMPOSE_PROJECT_NAME with compose.server-managed.yaml, so an orphan sweep
 *      stops and removes aimee-server itself — the container running the deploy);
 *   2. the legacy pre-baked aimee-llm-cpu container is retired, so it cannot keep
 *      answering to the `aimee-llm` network name alongside the one LLM service.
 *
 * deploy_apply.c is included directly to reach its static helpers; the two config
 * symbols it calls are stubbed so the test needs no database. */

/* deploy_apply.c calls execvpe(); its own _GNU_SOURCE lands after this file's
 * includes have already pulled in <features.h>, so declare it up front here. */
#define _GNU_SOURCE 1

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "config_database.h"

/* --- stubs for the config surface deploy_apply.c pulls in --- */

static char g_stub_profiles[64] = "kb,llm";
static char g_stub_random_hex = 'a';

const char *aimee_home(void)
{
   return getenv("AIMEE_HOME");
}

int platform_random_hex(char *out, size_t hex_len)
{
   memset(out, g_stub_random_hex, hex_len);
   out[hex_len] = '\0';
   return 0;
}

int config_load(config_t *cfg)
{
   if (cfg)
      memset(cfg, 0, sizeof(*cfg));
   return 0;
}

void config_emit_deploy_env(const config_t *cfg, char *buf, size_t n)
{
   (void)cfg;
   if (buf && n)
      snprintf(buf, n, "COMPOSE_PROFILES=%s\n", g_stub_profiles);
}

#include "../server/deploy_apply.c"

static const char *envp_value(char **envp, const char *key)
{
   size_t n = strlen(key);
   for (size_t i = 0; envp && envp[i]; i++)
      if (strncmp(envp[i], key, n) == 0 && envp[i][n] == '=')
         return envp[i] + n + 1;
   return NULL;
}

static int envp_key_count(char **envp, const char *key)
{
   int count = 0;
   size_t n = strlen(key);
   for (size_t i = 0; envp && envp[i]; i++)
      if (strncmp(envp[i], key, n) == 0 && envp[i][n] == '=')
         count++;
   return count;
}

static void test_managed_llm_service_credential(void)
{
   char tmp[] = "/tmp/aimee-deploy-llm-token-XXXXXX";
   assert(mkdtemp(tmp) != NULL);
   assert(setenv("AIMEE_HOME", tmp, 1) == 0);
   unsetenv("AIMEE_LLM_AUTH_TOKEN");
   unsetenv("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE");
   snprintf(g_stub_profiles, sizeof(g_stub_profiles), "kb,llm");

   g_stub_random_hex = 'a';
   int managed_llm = 0;
   int managed_identity = 0;
   char **envp = build_deploy_envp(NULL, 0, &managed_llm, &managed_identity);
   assert(envp != NULL);
   assert(managed_llm == 1);
   assert(managed_identity == 1);
   const char *token = envp_value(envp, "AIMEE_LLM_AUTH_TOKEN");
   assert(token != NULL && strlen(token) == 64);
   for (size_t i = 0; i < 64; i++)
      assert(token[i] == 'a');
   assert(envp_key_count(envp, "AIMEE_LLM_AUTH_TOKEN") == 1);
   assert(strcmp(envp_value(envp, "AIMEE_LLM_AUTH_REQUIRED"), "1") == 0);
   assert(envp_key_count(envp, "AIMEE_LLM_AUTH_REQUIRED") == 1);
   assert(setenv("COMPOSE_PROFILES", "attacker-profile", 1) == 0);
   free_envp(envp);
   envp = build_deploy_envp(NULL, 0, NULL, NULL);
   assert(strcmp(envp_value(envp, "COMPOSE_PROFILES"), "kb,llm") == 0);
   assert(envp_key_count(envp, "COMPOSE_PROFILES") == 1);
   free_envp(envp);

   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/%s", tmp, DEPLOY_LLM_TOKEN_FILE);
   struct stat st;
   assert(stat(path, &st) == 0 && S_ISREG(st.st_mode));
   assert((st.st_mode & 0777) == 0600);

   /* Re-apply reads the persisted identity instead of silently rotating it. */
   g_stub_random_hex = 'b';
   envp = build_deploy_envp(NULL, 0, NULL, NULL);
   token = envp_value(envp, "AIMEE_LLM_AUTH_TOKEN");
   assert(token != NULL && token[0] == 'a');
   free_envp(envp);

   /* Inherited empty OR non-empty child state cannot shadow the managed file. */
   assert(setenv("AIMEE_LLM_AUTH_TOKEN", "stale-inherited-service-token-1234", 1) == 0);
   assert(setenv("AIMEE_LLM_AUTH_REQUIRED", "0", 1) == 0);
   envp = build_deploy_envp(NULL, 0, NULL, NULL);
   assert(envp_key_count(envp, "AIMEE_LLM_AUTH_TOKEN") == 1);
   token = envp_value(envp, "AIMEE_LLM_AUTH_TOKEN");
   assert(token != NULL && token[0] == 'a');
   assert(strcmp(envp_value(envp, "AIMEE_LLM_AUTH_REQUIRED"), "1") == 0);
   assert(envp_key_count(envp, "AIMEE_LLM_AUTH_REQUIRED") == 1);
   free_envp(envp);

   /* A distinctly named, deliberate operator override wins exactly once. */
   assert(setenv("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE", "operator-managed-service-token-1234",
                 1) == 0);
   envp = build_deploy_envp(NULL, 0, NULL, NULL);
   assert(strcmp(envp_value(envp, "AIMEE_LLM_AUTH_TOKEN"), "operator-managed-service-token-1234") ==
          0);
   assert(envp_key_count(envp, "AIMEE_LLM_AUTH_TOKEN") == 1);
   free_envp(envp);
   assert(setenv("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE", "invalid token with spaces", 1) == 0);
   assert(build_deploy_envp(NULL, 0, NULL, NULL) == NULL);
   unsetenv("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE");

   /* The persisted authority must remain private and cannot be a symlink. */
   assert(chmod(path, 0644) == 0);
   assert(build_deploy_envp(NULL, 0, NULL, NULL) == NULL);
   assert(chmod(path, 0600) == 0);
   char real_path[PATH_MAX];
   snprintf(real_path, sizeof(real_path), "%s.real", path);
   assert(rename(path, real_path) == 0);
   assert(symlink(real_path, path) == 0);
   assert(build_deploy_envp(NULL, 0, NULL, NULL) == NULL);
   assert(unlink(path) == 0);
   assert(rename(real_path, path) == 0);

   /* No local LLM means no credential is invented or passed. */
   unsetenv("AIMEE_LLM_AUTH_TOKEN");
   unsetenv("AIMEE_LLM_AUTH_REQUIRED");
   snprintf(g_stub_profiles, sizeof(g_stub_profiles), "kb");
   managed_llm = 1;
   managed_identity = 0;
   envp = build_deploy_envp(NULL, 0, &managed_llm, &managed_identity);
   assert(envp != NULL && envp_value(envp, "AIMEE_LLM_AUTH_TOKEN") == NULL);
   assert(envp_value(envp, "AIMEE_LLM_AUTH_REQUIRED") == NULL);
   assert(managed_llm == 0);
   assert(managed_identity == 1);
   free_envp(envp);

   /* A complete explicit packet wins; a partial packet is never mixed with a
    * wizard-generated identity. */
   assert(setenv("AIMEE_KB_CONN", "aimee://kb:8745?ca=sha256:x&enroll=x", 1) == 0);
   assert(setenv("AIMEE_SERVER_ID", "operator-server", 1) == 0);
   assert(setenv("AIMEE_SERVER_TEAM_ID", "7", 1) == 0);
   envp = build_deploy_envp(NULL, 0, NULL, &managed_identity);
   assert(envp != NULL && managed_identity == 0);
   free_envp(envp);
   unsetenv("AIMEE_SERVER_TEAM_ID");
   assert(build_deploy_envp(NULL, 0, NULL, NULL) == NULL);
   unsetenv("AIMEE_KB_CONN");
   unsetenv("AIMEE_SERVER_ID");

   assert(unlink(path) == 0);
   assert(rmdir(tmp) == 0);
   unsetenv("AIMEE_HOME");
   unsetenv("COMPOSE_PROFILES");
   unsetenv("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE");
   unsetenv("AIMEE_SERVER_TEAM_ID");
   snprintf(g_stub_profiles, sizeof(g_stub_profiles), "kb,llm");
   printf("  managed kb -> llm credential is stable, private, and scoped ok\n");
}

/* --- the deploy argv itself --- */

static void test_deploy_argv_has_no_remove_orphans(void)
{
   char file[512];
   deploy_apply_compose_file(file, sizeof(file));

   const char *argv[8];
   int n = deploy_up_argv(file, argv, sizeof(argv) / sizeof(argv[0]));
   assert(n == 6);
   assert(argv[n] == NULL);

   /* The regression: --remove-orphans made compose stop and remove aimee-server,
    * because the managed compose runs in the same project and does not declare it. */
   for (int i = 0; i < n; i++)
      assert(strcmp(argv[i], "--remove-orphans") != 0);

   assert(strcmp(argv[0], "docker") == 0);
   assert(strcmp(argv[1], "compose") == 0);
   assert(strcmp(argv[2], "-f") == 0);
   assert(strcmp(argv[3], file) == 0);
   assert(strcmp(argv[4], "up") == 0);
   assert(strcmp(argv[5], "-d") == 0);

   /* a buffer with no room for the NULL terminator is refused, not overrun */
   const char *tight[6];
   assert(deploy_up_argv(file, tight, 6) == -1);
   assert(deploy_up_argv(file, NULL, 8) == -1);
   printf("  deploy argv omits --remove-orphans ok\n");
}

static void test_llm_probe_uses_kb_credential_without_host_secret(void)
{
   const char *argv[12];
   int n = deploy_llm_probe_argv("/managed.yaml", argv, sizeof(argv) / sizeof(argv[0]));
   assert(n == 10 && argv[n] == NULL);
   assert(strcmp(argv[0], "docker") == 0 && strcmp(argv[1], "compose") == 0);
   assert(strcmp(argv[3], "/managed.yaml") == 0 && strcmp(argv[4], "exec") == 0);
   assert(strcmp(argv[6], "aimee-kb") == 0 && strcmp(argv[7], "sh") == 0);
   assert(strstr(argv[9], "AIMEE_LLM_URL") != NULL);
   assert(strstr(argv[9], "AIMEE_LLM_AUTH_TOKEN") != NULL);
   assert(strstr(argv[9], "/auth/verify") != NULL);
   assert(strstr(argv[9], "operator-managed-service-token") == NULL);
   assert(deploy_llm_probe_argv("/managed.yaml", argv, 10) == -1);
   printf("  deploy verifies the KB's authenticated LLM connection without host secret argv ok\n");
}

/* --- wizard-managed server workload identity --- */

static void test_managed_identity_bootstrap_runs_inside_kb_without_secret_argv(void)
{
   const char *argv[16];
   int n = deploy_identity_bootstrap_argv("/managed.yaml", argv,
                                          sizeof(argv) / sizeof(argv[0]));
   assert(n > 0 && argv[n] == NULL);
   assert(strcmp(argv[0], "docker") == 0 && strcmp(argv[1], "compose") == 0);
   assert(strcmp(argv[3], "/managed.yaml") == 0 && strcmp(argv[4], "run") == 0);

   int saw_bootstrap = 0;
   for (int i = 0; i < n; i++)
   {
      assert(strstr(argv[i], "enroll=") == NULL);
      assert(strstr(argv[i], "PRIVATE KEY") == NULL);
      assert(strcmp(argv[i], "--no-deps") != 0);
      if (strcmp(argv[i], "aimee-server-identity") == 0)
         saw_bootstrap = 1;
   }
   assert(saw_bootstrap);
   assert(deploy_identity_bootstrap_argv("/managed.yaml", argv, (size_t)n) == -1);
   printf("  deploy invokes the KB-owned managed identity bootstrap without host secret argv ok\n");
}

/* --- the legacy CPU container is retired by name --- */

static void test_retire_targets_legacy_cpu_container(void)
{
   /* aimee-llm-cpu is no longer a service of the managed compose file, so `up`
    * cannot touch it and `docker compose rm <service>` would not find it. It has
    * to be removed by CONTAINER name, or it keeps holding the `aimee-llm` network
    * alias next to the real LLM service and the kb can reach the stale one.
    *
    * Assert the command, not the effect of running it: the retirement execs
    * docker, and whether a docker exists differs between a dev box and CI. */
   const char *argv[8];
   int n = deploy_retire_argv(argv, sizeof(argv) / sizeof(argv[0]));
   assert(n == 4);
   assert(argv[n] == NULL);
   assert(strcmp(argv[0], "docker") == 0);
   assert(strcmp(argv[1], "rm") == 0);
   assert(strcmp(argv[2], "-f") == 0);
   /* the container name, NOT the compose service name */
   assert(strcmp(argv[3], "aimee-aimee-llm-cpu-1") == 0);

   /* `docker compose rm` would be wrong here — the service no longer exists. */
   for (int i = 0; i < n; i++)
      assert(strcmp(argv[i], "compose") != 0);

   /* a buffer with no room for the NULL terminator is refused, not overrun */
   const char *tight[4];
   assert(deploy_retire_argv(tight, 4) == -1);
   assert(deploy_retire_argv(NULL, 8) == -1);
   printf("  legacy cpu retirement targets the container by name ok\n");
}

/* --- compose file resolution --- */

static void test_compose_file_default(void)
{
   char file[512];
   deploy_apply_compose_file(file, sizeof(file));
   assert(file[0] == '/');
   assert(strstr(file, "aimee-managed.compose.yaml") != NULL);
   printf("  compose file default ok\n");
}

int main(void)
{
   printf("test_deploy_apply\n");
   test_managed_llm_service_credential();
   test_deploy_argv_has_no_remove_orphans();
   test_llm_probe_uses_kb_credential_without_host_secret();
   test_managed_identity_bootstrap_runs_inside_kb_without_secret_argv();
   test_retire_targets_legacy_cpu_container();
   test_compose_file_default();
   printf("test_deploy_apply: all passed\n");
   return 0;
}
