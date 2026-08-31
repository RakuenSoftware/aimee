/* config_workspace_add: prune workspaces whose path is gone before refusing.
 *
 * The registry holds 64 entries and had no removal path other than an explicit
 * `aimee workspace remove`. Any workflow that creates short-lived checkouts --
 * CI jobs, ephemeral worktrees, benchmark cells -- fills all 64 and then EVERY
 * registration fails with "maximum workspace count reached (64)", on a machine
 * where none of the 64 still exist on disk.
 *
 * Observed: an overnight benchmark run died on exactly this. Three cells had
 * already completed and been cleaned up; the fourth could not register its
 * workspace and the task never ran.
 *
 * Only entries whose path is GONE are collected. A workspace on an unmounted
 * volume must survive: stat failing for any reason other than ENOENT leaves the
 * entry alone, because "I cannot see it right now" is not "it does not exist".
 */
#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static char g_home[256];

static void set_home(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee-wsprune-XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_home) != NULL);
   setenv("AIMEE_HOME", g_home, 1);
   setenv("HOME", g_home, 1);
}

static void make_dir(char *out, size_t n, int i)
{
   snprintf(out, n, "%s/live-%d", g_home, i);
   assert(mkdir(out, 0700) == 0);
}

/* Fill the registry with entries whose directories are then deleted, and assert
 * a fresh add still succeeds. Against the old code every one of these fails. */
static void test_full_registry_of_dead_paths_still_accepts_an_add(void)
{
   char path[512];
   int cap = 64;
   for (int i = 0; i < cap; i++)
   {
      make_dir(path, sizeof(path), i);
      assert(config_workspace_add(path, NULL, NULL, NULL) == 0);
      /* The checkout goes away, as a benchmark cell's does. */
      assert(rmdir(path) == 0);
   }
   /* Registry is nominally full; every entry is a corpse. */
   char fresh[512];
   snprintf(fresh, sizeof(fresh), "%s/fresh", g_home);
   assert(mkdir(fresh, 0700) == 0);
   int rc = config_workspace_add(fresh, NULL, NULL, NULL);
   assert(rc == 0); /* -3 == "maximum workspace count reached", the bug */
}

/* A live workspace must never be collected just because the registry is full. */
static void test_live_workspaces_survive_the_prune(void)
{
   char live[512];
   snprintf(live, sizeof(live), "%s/keepme", g_home);
   assert(mkdir(live, 0700) == 0);
   assert(config_workspace_add(live, NULL, NULL, NULL) == 0);

   char path[512];
   for (int i = 0; i < 70; i++)
   {
      snprintf(path, sizeof(path), "%s/churn-%d", g_home, i);
      if (mkdir(path, 0700) != 0)
         continue;
      config_workspace_add(path, NULL, NULL, NULL);
      rmdir(path);
   }
   /* Re-adding the live one reports "already registered" (-2), proving it is
    * still in the registry rather than having been pruned. */
   assert(config_workspace_add(live, NULL, NULL, NULL) == -2);
}

int main(void)
{
   printf("workspace_prune_dead: ");
   set_home();
   test_full_registry_of_dead_paths_still_accepts_an_add();
   test_live_workspaces_survive_the_prune();
   printf("ok\n");
   return 0;
}
