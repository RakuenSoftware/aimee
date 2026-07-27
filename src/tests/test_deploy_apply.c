/* test_deploy_apply.c — the managed-deploy command construction.
 *
 * Covers the two properties the wizard's Deploy depends on:
 *   1. the deploy never passes --remove-orphans (the managed compose shares
 *      COMPOSE_PROJECT_NAME with compose.server-managed.yaml, so an orphan sweep
 *      stops and removes aimee-server itself — the container running the deploy);
 *   2. the LLM variant that was NOT selected is retired, so the mutually-exclusive
 *      GPU/CPU services never both answer to the `aimee-llm` network name.
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

static char g_stub_profiles[64] = "kb,llm-cpu";

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

/* --- profiles_select: whole-entry matching, not substring --- */

static void test_profiles_select(void)
{
   assert(profiles_select("kb,llm-cpu", "llm-cpu") == 1);
   assert(profiles_select("kb,llm", "llm") == 1);
   assert(profiles_select("kb", "llm") == 0);
   assert(profiles_select("", "llm") == 0);
   assert(profiles_select(NULL, "llm") == 0);

   /* The regression that a substring match would cause: "llm-cpu" must NOT read as
    * the GPU profile "llm", or the deploy would retire the variant it just brought
    * up and leave the stale one running. */
   assert(profiles_select("kb,llm-cpu", "llm") == 0);
   assert(profiles_select("llm-cpu", "llm") == 0);

   /* leading/trailing entries and a single entry */
   assert(profiles_select("llm,kb", "llm") == 1);
   assert(profiles_select("kb,llm", "kb") == 1);
   assert(profiles_select("llm", "llm") == 1);
   printf("  profiles_select ok\n");
}

/* --- envp_get --- */

static void test_envp_get(void)
{
   char *envp[] = {(char *)"PATH=/bin", (char *)"COMPOSE_PROFILES=kb,llm", (char *)"X=1", NULL};
   assert(strcmp(envp_get(envp, "COMPOSE_PROFILES"), "kb,llm") == 0);
   assert(strcmp(envp_get(envp, "PATH"), "/bin") == 0);
   assert(envp_get(envp, "MISSING") == NULL);
   /* a prefix of a real key must not match */
   assert(envp_get(envp, "COMPOSE") == NULL);
   assert(envp_get(NULL, "PATH") == NULL);
   printf("  envp_get ok\n");
}

/* --- envp_with_profile: replaces, never appends a second entry --- */

static void test_envp_with_profile(void)
{
   char *envp[] = {(char *)"PATH=/bin", (char *)"COMPOSE_PROFILES=kb,llm-cpu", (char *)"X=1", NULL};
   char **out = envp_with_profile(envp, "llm");
   assert(out != NULL);

   int profiles_seen = 0, path_seen = 0, x_seen = 0;
   for (size_t i = 0; out[i]; i++)
   {
      if (strncmp(out[i], "COMPOSE_PROFILES=", 17) == 0)
      {
         profiles_seen++;
         /* the forced value wins — a duplicate entry would leave the child's
          * getenv() free to return the deploy's own selection instead */
         assert(strcmp(out[i], "COMPOSE_PROFILES=llm") == 0);
      }
      else if (strcmp(out[i], "PATH=/bin") == 0)
         path_seen = 1;
      else if (strcmp(out[i], "X=1") == 0)
         x_seen = 1;
   }
   assert(profiles_seen == 1);
   assert(path_seen && x_seen);
   free_envp(out);

   /* an env with no COMPOSE_PROFILES still gains exactly one */
   char *bare[] = {(char *)"PATH=/bin", NULL};
   out = envp_with_profile(bare, "llm-cpu");
   assert(out != NULL);
   assert(strcmp(envp_get(out, "COMPOSE_PROFILES"), "llm-cpu") == 0);
   free_envp(out);
   printf("  envp_with_profile ok\n");
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
   test_profiles_select();
   test_envp_get();
   test_envp_with_profile();
   test_deploy_argv_has_no_remove_orphans();
   test_compose_file_default();
   printf("test_deploy_apply: all passed\n");
   return 0;
}
