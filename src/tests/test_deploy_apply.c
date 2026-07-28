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
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "config_database.h"

/* --- stubs for the config surface deploy_apply.c pulls in --- */

static char g_stub_profiles[64] = "kb,llm";

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
   test_deploy_argv_has_no_remove_orphans();
   test_retire_targets_legacy_cpu_container();
   test_compose_file_default();
   printf("test_deploy_apply: all passed\n");
   return 0;
}
