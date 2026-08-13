/* test_deploy_apply.c — the managed-deploy command construction.
 *
 * Covers the managed deployment properties the wizard depends on:
 *   1. the deploy never passes --remove-orphans (the managed compose shares
 *      COMPOSE_PROJECT_NAME with compose.server-managed.yaml, so an orphan sweep
 *      stops and removes aimee-server itself — the container running the deploy);
 *   2. KB and LLM have separate --no-deps start commands, allowing the worker to
 *      start KB first without waiting for LLM model readiness;
 *   3. the legacy pre-baked aimee-llm-cpu container is retired, so it cannot keep
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
#include "runtime_secret.h"
#include "vault_config_bootstrap.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

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

/* deploy_apply asks config_present() then config_emit_deploy_env_current(); the
 * config_load + config_emit_deploy_env pair these replace behaved the same way. */
int config_present(void)
{
   return 1;
}

/* Emitted alongside the profiles because a managed kb without an embedder is refused.
 * Both keys are always emitted, empty when unset — which is what the real emitter does,
 * and why presence alone cannot be the test. */
static const char *g_stub_embedder_model = "bekko-a25m";
static const char *g_stub_embedder_url = "";

void config_emit_deploy_env_current(char *buf, size_t n)
{
   if (buf && n)
      snprintf(buf, n, "COMPOSE_PROFILES=%s\nEMBEDDER_MODEL=%s\nEMBEDDER_URL=%s\n", g_stub_profiles,
               g_stub_embedder_model, g_stub_embedder_url);
}

/* Keyless Vault double: persistence semantics are exercised through the same
 * bounded runtime cache while encrypted-store behavior stays in Vault tests. */
int vault_runtime_secret_set(const char *name, const char *value)
{
   return runtime_secret_store(name, value);
}

int vault_runtime_secret_delete(const char *name)
{
   runtime_secret_remove(name);
   return 0;
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
   char tmp[256];
   snprintf(tmp, sizeof tmp, "%s/aimee-deploy-llm-token-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmp) != NULL);
   assert(setenv("AIMEE_HOME", tmp, 1) == 0);
   runtime_secret_remove("SYNTHESIS_API_KEY");
   runtime_secret_remove("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE");
   unsetenv("SYNTHESIS_API_KEY");
   unsetenv("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE");
   snprintf(g_stub_profiles, sizeof(g_stub_profiles), "kb,llm");

   g_stub_random_hex = 'a';
   int managed_llm = 0;
   int managed_kb = 0;
   int managed_identity = 0;
   char **envp = build_deploy_envp(NULL, 0, &managed_llm, &managed_kb, &managed_identity);
   assert(envp != NULL);
   assert(managed_llm == 1);
   assert(managed_kb == 1);
   assert(managed_identity == 1);
   const char *token = envp_value(envp, "SYNTHESIS_API_KEY");
   assert(token != NULL && strlen(token) == 64);
   for (size_t i = 0; i < 64; i++)
      assert(token[i] == 'a');
   assert(envp_key_count(envp, "SYNTHESIS_API_KEY") == 1);
   assert(strcmp(envp_value(envp, "SYNTHESIS_AUTH_REQUIRED"), "1") == 0);
   assert(envp_key_count(envp, "SYNTHESIS_AUTH_REQUIRED") == 1);
   assert(setenv("COMPOSE_PROFILES", "attacker-profile", 1) == 0);
   free_envp(envp);
   envp = build_deploy_envp(NULL, 0, NULL, NULL, NULL);
   assert(strcmp(envp_value(envp, "COMPOSE_PROFILES"), "kb,llm") == 0);
   assert(envp_key_count(envp, "COMPOSE_PROFILES") == 1);
   free_envp(envp);

   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/%s", tmp, DEPLOY_LLM_TOKEN_FILE);
   struct stat st;
   assert(stat(path, &st) != 0 && errno == ENOENT);

   /* Re-apply reads the vaulted identity instead of silently rotating it. */
   g_stub_random_hex = 'b';
   envp = build_deploy_envp(NULL, 0, NULL, NULL, NULL);
   token = envp_value(envp, "SYNTHESIS_API_KEY");
   assert(token != NULL && token[0] == 'a');
   free_envp(envp);

   /* Inherited empty OR non-empty child state cannot shadow the managed file. */
   assert(setenv("SYNTHESIS_API_KEY", "stale-inherited-service-token-1234", 1) == 0);
   assert(setenv("SYNTHESIS_AUTH_REQUIRED", "0", 1) == 0);
   envp = build_deploy_envp(NULL, 0, NULL, NULL, NULL);
   assert(envp_key_count(envp, "SYNTHESIS_API_KEY") == 1);
   token = envp_value(envp, "SYNTHESIS_API_KEY");
   assert(token != NULL && token[0] == 'a');
   assert(strcmp(envp_value(envp, "SYNTHESIS_AUTH_REQUIRED"), "1") == 0);
   assert(envp_key_count(envp, "SYNTHESIS_AUTH_REQUIRED") == 1);
   free_envp(envp);

   /* First-boot override input is already in the cache by the time deploy runs;
    * applying it seals the child credential and makes it authoritative. */
   assert(runtime_secret_store("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE",
                               "operator-managed-service-token-1234") == 0);
   envp = build_deploy_envp(NULL, 0, NULL, NULL, NULL);
   assert(strcmp(envp_value(envp, "SYNTHESIS_API_KEY"), "operator-managed-service-token-1234") ==
          0);
   assert(envp_key_count(envp, "SYNTHESIS_API_KEY") == 1);
   free_envp(envp);
   runtime_secret_remove("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE");
   assert(runtime_secret_store("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE",
                               "invalid token with spaces") == 0);
   assert(build_deploy_envp(NULL, 0, NULL, NULL, NULL) == NULL);
   runtime_secret_remove("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE");

   /* A historical pre-Vault file is accepted only as one-shot migration input:
    * insecure modes and symlinks fail closed; a private regular file is sealed
    * and then erased. */
   runtime_secret_remove("SYNTHESIS_API_KEY");
   int legacy_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
   assert(legacy_fd >= 0);
   const char *legacy = "legacy-managed-service-token-1234";
   assert(write(legacy_fd, legacy, strlen(legacy)) == (ssize_t)strlen(legacy));
   assert(close(legacy_fd) == 0);
   assert(chmod(path, 0644) == 0);
   assert(build_deploy_envp(NULL, 0, NULL, NULL, NULL) == NULL);
   assert(chmod(path, 0600) == 0);
   char real_path[PATH_MAX];
   snprintf(real_path, sizeof(real_path), "%s.real", path);
   assert(rename(path, real_path) == 0);
   assert(symlink(real_path, path) == 0);
   assert(build_deploy_envp(NULL, 0, NULL, NULL, NULL) == NULL);
   assert(unlink(path) == 0);
   assert(rename(real_path, path) == 0);
   envp = build_deploy_envp(NULL, 0, NULL, NULL, NULL);
   assert(envp != NULL);
   assert(strcmp(envp_value(envp, "SYNTHESIS_API_KEY"), legacy) == 0);
   free_envp(envp);
   assert(stat(path, &st) != 0 && errno == ENOENT);

   /* No local LLM means no credential is invented or passed. */
   unsetenv("SYNTHESIS_API_KEY");
   unsetenv("SYNTHESIS_AUTH_REQUIRED");
   snprintf(g_stub_profiles, sizeof(g_stub_profiles), "kb");
   managed_llm = 1;
   managed_identity = 0;
   envp = build_deploy_envp(NULL, 0, &managed_llm, &managed_kb, &managed_identity);
   assert(envp != NULL && envp_value(envp, "SYNTHESIS_API_KEY") == NULL);
   assert(envp_value(envp, "SYNTHESIS_AUTH_REQUIRED") == NULL);
   assert(managed_llm == 0);
   assert(managed_kb == 1);
   assert(managed_identity == 1);
   free_envp(envp);

   /* A complete explicit packet wins; a partial packet is never mixed with a
    * wizard-generated identity. */
   assert(runtime_secret_store("AIMEE_KB_CONN", "aimee://kb:8745?ca=sha256:x&enroll=x") == 0);
   assert(setenv("AIMEE_SERVER_ID", "operator-server", 1) == 0);
   assert(setenv("AIMEE_SERVER_TEAM_ID", "7", 1) == 0);
   envp = build_deploy_envp(NULL, 0, NULL, NULL, &managed_identity);
   assert(envp != NULL && managed_identity == 0);
   free_envp(envp);
   unsetenv("AIMEE_SERVER_TEAM_ID");
   assert(build_deploy_envp(NULL, 0, NULL, NULL, NULL) == NULL);
   runtime_secret_remove("AIMEE_KB_CONN");
   unsetenv("AIMEE_SERVER_ID");

   runtime_secret_remove("SYNTHESIS_API_KEY");
   runtime_secret_remove("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE");
   assert(rmdir(tmp) == 0);
   unsetenv("AIMEE_HOME");
   unsetenv("COMPOSE_PROFILES");
   unsetenv("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE");
   unsetenv("AIMEE_SERVER_TEAM_ID");
   snprintf(g_stub_profiles, sizeof(g_stub_profiles), "kb,llm");
   printf("  managed kb -> llm credential is stable, private, and scoped ok\n");
}

/* A managed kb with no embedder is refused before anything is started. There is no
 * fallback embedder left to come up with instead: the container would print why it
 * cannot serve retrieval and exit, so the deploy fails either way. It fails here
 * because this is where the wizard shows the reason, rather than a container log. */
static void test_managed_kb_without_an_embedder_is_refused(void)
{
   char err[256];
   char **envp;
   snprintf(g_stub_profiles, sizeof(g_stub_profiles), "kb");
   g_stub_embedder_model = "";
   g_stub_embedder_url = "";

   err[0] = '\0';
   assert(build_deploy_envp(err, sizeof(err), NULL, NULL, NULL) == NULL);
   assert(strstr(err, "no embedder selected") != NULL);
   /* The message has to name a way out, not just the problem. */
   assert(strstr(err, "embedder_model") != NULL);
   assert(strstr(err, "EMBEDDER_URL") != NULL);

   /* A bundled model satisfies it. */
   g_stub_embedder_model = "bekko-a25m";
   envp = build_deploy_envp(err, sizeof(err), NULL, NULL, NULL);
   assert(envp != NULL);
   free_envp(envp);

   /* So does an external endpoint, on its own. */
   g_stub_embedder_model = "";
   g_stub_embedder_url = "http://embedder.example:8760";
   envp = build_deploy_envp(err, sizeof(err), NULL, NULL, NULL);
   assert(envp != NULL);
   free_envp(envp);

   /* A remote kb deploys no container, so the check does not apply to it. */
   g_stub_profiles[0] = '\0';
   g_stub_embedder_model = "";
   g_stub_embedder_url = "";
   envp = build_deploy_envp(err, sizeof(err), NULL, NULL, NULL);
   assert(envp != NULL);
   free_envp(envp);

   snprintf(g_stub_profiles, sizeof(g_stub_profiles), "kb,llm");
   g_stub_embedder_model = "bekko-a25m";
   g_stub_embedder_url = "";
   printf("  managed kb with no embedder is refused, with a way out named ok\n");
}

/* --- the deploy argv itself --- */

static void test_deploy_argv_is_orderable_and_has_no_remove_orphans(void)
{
   char file[512];
   deploy_apply_compose_file(file, sizeof(file));

   const char *argv[10];
   int n = deploy_up_service_argv(file, "aimee-kb", argv, sizeof(argv) / sizeof(argv[0]));
   assert(n == 8);
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
   assert(strcmp(argv[6], "--no-deps") == 0);
   assert(strcmp(argv[7], "aimee-kb") == 0);

   n = deploy_up_service_argv(file, "aimee-llm", argv, sizeof(argv) / sizeof(argv[0]));
   assert(n == 8 && strcmp(argv[7], "aimee-llm") == 0);

   /* a buffer with no room for the NULL terminator is refused, not overrun */
   const char *tight[8];
   assert(deploy_up_service_argv(file, "aimee-kb", tight, 8) == -1);
   assert(deploy_up_service_argv(file, "aimee-kb", NULL, 10) == -1);
   assert(deploy_up_service_argv(file, "", argv, 10) == -1);
   printf("  deploy argv supports explicit KB-then-LLM ordering without orphan removal ok\n");
}

static void test_managed_kb_credential_bootstrap_is_stdin_only(void)
{
   const char *argv[16];
   int n = deploy_kb_vault_bootstrap_argv("/managed.yaml", argv, sizeof(argv) / sizeof(argv[0]));
   assert(n > 0 && argv[n] == NULL);
   assert(strcmp(argv[0], "docker") == 0 && strcmp(argv[1], "compose") == 0);
   assert(strcmp(argv[3], "/managed.yaml") == 0 && strcmp(argv[4], "run") == 0);
   int saw_rm = 0, saw_stdin_bootstrap = 0, saw_kb = 0;
   for (int i = 0; i < n; i++)
   {
      assert(strstr(argv[i], "SYNTHESIS_API_KEY=") == NULL);
      assert(strstr(argv[i], "Bearer ") == NULL);
      if (strcmp(argv[i], "--rm") == 0)
         saw_rm = 1;
      if (strcmp(argv[i], "--bootstrap-vault-stdin") == 0)
         saw_stdin_bootstrap = 1;
      if (strcmp(argv[i], "aimee-kb") == 0)
         saw_kb = 1;
   }
   assert(saw_rm && saw_stdin_bootstrap && saw_kb);
   assert(deploy_kb_vault_bootstrap_argv("/managed.yaml", argv, (size_t)n) == -1);
   printf("  managed KB service credential crosses only a disposable stdin bootstrap ok\n");
}

/* --- wizard-managed server workload identity --- */

static void test_managed_identity_bootstrap_runs_inside_kb_without_secret_argv(void)
{
   const char *argv[16];
   int n = deploy_identity_bootstrap_argv("/managed.yaml", argv, sizeof(argv) / sizeof(argv[0]));
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

static void test_managed_authority_bootstrap_is_isolated_and_secret_free(void)
{
   const char *argv[16];
   int n = deploy_authority_bootstrap_argv("/managed.yaml", argv, sizeof(argv) / sizeof(argv[0]));
   assert(n > 0 && argv[n] == NULL);
   assert(strcmp(argv[0], "docker") == 0 && strcmp(argv[1], "compose") == 0);
   assert(strcmp(argv[3], "/managed.yaml") == 0 && strcmp(argv[4], "run") == 0);

   int saw_bootstrap = 0;
   for (int i = 0; i < n; i++)
   {
      assert(strstr(argv[i], "PRIVATE KEY") == NULL);
      assert(strstr(argv[i], "KMS_KEY") == NULL);
      assert(strstr(argv[i], "postgresql://") == NULL);
      assert(strcmp(argv[i], "--no-deps") != 0);
      if (strcmp(argv[i], "aimee-authority-bootstrap") == 0)
         saw_bootstrap = 1;
   }
   assert(saw_bootstrap);
   assert(deploy_authority_bootstrap_argv("/managed.yaml", argv, (size_t)n) == -1);
   printf("  deploy invokes isolated authority bootstrap without host secret argv ok\n");
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

/* --- `docker compose ps` is scoped to the managed services --- */

/* `ps` scopes to COMPOSE_PROJECT_NAME, not to the -f file, so it also reports
 * aimee-server (started by compose.server-managed.yaml under the same project).
 * The wizard counts these entries to choose its button label, so leaving the
 * orchestrator in the list made a never-deployed box offer "Re-deploy". Labels
 * are abbreviated to the one key the filter reads. */
#define PS_SERVER_ENTRY                                                                            \
   "{\"Name\":\"aimee-aimee-server-1\",\"State\":\"running\",\"Labels\":\"com.docker.compose."     \
   "project=aimee,com.docker.compose.project.config_files=/opt/aimee-src/compose.server-managed."  \
   "yaml\"}"
#define PS_KB_ENTRY                                                                                \
   "{\"Name\":\"aimee-aimee-kb-1\",\"State\":\"running\",\"Labels\":\"com.docker.compose."         \
   "project=aimee,com.docker.compose.project.config_files=/opt/aimee/deploy/"                      \
   "aimee-managed.compose."                                                                        \
   "yaml\"}"

static const char *k_managed = "/opt/aimee/deploy/aimee-managed.compose.yaml";

static void test_ps_drops_the_orchestrator_from_the_service_list(void)
{
   /* NDJSON shape (newer compose). */
   char ps[4096];
   snprintf(ps, sizeof(ps), "%s\n%s\n", PS_KB_ENTRY, PS_SERVER_ENTRY);
   deploy_filter_managed_ps(ps, sizeof(ps), k_managed);
   assert(strstr(ps, "aimee-aimee-kb-1") != NULL);
   assert(strstr(ps, "aimee-aimee-server-1") == NULL);

   /* JSON array shape (older compose). */
   snprintf(ps, sizeof(ps), "[%s,%s]", PS_SERVER_ENTRY, PS_KB_ENTRY);
   deploy_filter_managed_ps(ps, sizeof(ps), k_managed);
   assert(strstr(ps, "aimee-aimee-kb-1") != NULL);
   assert(strstr(ps, "aimee-aimee-server-1") == NULL);
   printf("  ps drops the orchestrator from the managed service list ok\n");
}

/* The regression itself: before any deploy, the only container in project
 * "aimee" is the server running the wizard. That has to leave an EMPTY list, or
 * the finish screen labels its button "Re-deploy" for a KB that does not exist. */
static void test_ps_is_empty_before_the_first_deploy(void)
{
   char ps[4096];
   snprintf(ps, sizeof(ps), "%s\n", PS_SERVER_ENTRY);
   deploy_filter_managed_ps(ps, sizeof(ps), k_managed);
   assert(strstr(ps, "aimee-aimee-server-1") == NULL);
   /* an empty array, which parse_ps renders as zero services */
   assert(strcmp(ps, "[]") == 0);
   printf("  ps is empty before the first deploy ok\n");
}

/* A shape the filter cannot parse must pass through untouched — an unfiltered
 * list is recoverable, a wrongly-emptied one silently misreports the stack. */
static void test_ps_passes_through_unparseable_output(void)
{
   char ps[256];
   snprintf(ps, sizeof(ps), "not json at all");
   deploy_filter_managed_ps(ps, sizeof(ps), k_managed);
   assert(strcmp(ps, "not json at all") == 0);

   snprintf(ps, sizeof(ps), "   ");
   deploy_filter_managed_ps(ps, sizeof(ps), k_managed);
   assert(strcmp(ps, "   ") == 0);
   printf("  unparseable ps output passes through ok\n");
}

int main(void)
{
   printf("test_deploy_apply\n");
   test_managed_llm_service_credential();
   test_managed_kb_without_an_embedder_is_refused();
   test_deploy_argv_is_orderable_and_has_no_remove_orphans();
   test_managed_kb_credential_bootstrap_is_stdin_only();
   test_managed_identity_bootstrap_runs_inside_kb_without_secret_argv();
   test_managed_authority_bootstrap_is_isolated_and_secret_free();
   test_retire_targets_legacy_cpu_container();
   test_compose_file_default();
   test_ps_drops_the_orchestrator_from_the_service_list();
   test_ps_is_empty_before_the_first_deploy();
   test_ps_passes_through_unparseable_output();
   printf("test_deploy_apply: all passed\n");
   return 0;
}
