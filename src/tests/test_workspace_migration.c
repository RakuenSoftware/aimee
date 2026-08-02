/* test_workspace_migration.c — the legacy webusers -> environment migration.
 *
 * migrate_legacy_webusers() is the one piece of the single-tenant conversion
 * that MOVES USER DATA: a wrong destination silently loses a project. This
 * plants a realistic legacy tree — including the case the code has a dedicated
 * branch for, two actors owning the same project name with different content —
 * then drives the real ws_scope_environment_root() and checks that every entry
 * arrived somewhere, that nothing was overwritten, and that a second resolve is
 * a no-op.
 *
 * Note the migration does NOT promise WHICH colliding actor keeps the plain
 * name (it follows readdir order); it promises only that neither is lost and
 * that the renamed one is logged. The assertions below reflect that. */
#include "workspace_scope.h"

#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int PASS = 0, FAIL = 0;

static void ck(int cond, const char *what)
{
   if (cond)
   {
      printf("  PASS  %s\n", what);
      PASS++;
   }
   else
   {
      printf("  FAIL  %s\n", what);
      FAIL++;
   }
}

static int run(const char *fmt, ...)
{
   char cmd[2048];
   va_list ap;
   __builtin_va_start(ap, fmt);
   vsnprintf(cmd, sizeof(cmd), fmt, ap);
   __builtin_va_end(ap);
   return system(cmd);
}

/* Does `root` contain an entry whose name contains `frag`, holding a file whose
 * content matches `marker`? Returns 1 if found. Walks one level of org dirs too. */
static int found_marker(const char *root, const char *marker)
{
   char cmd[2048];
   snprintf(cmd, sizeof(cmd), "grep -rqs -- '%s' '%s' 2>/dev/null", marker, root);
   return system(cmd) == 0;
}

static int count_entries(const char *dir)
{
   DIR *d = opendir(dir);
   if (!d)
      return -1;
   int n = 0;
   struct dirent *e;
   while ((e = readdir(d)))
      if (e->d_name[0] != '.')
         n++;
   closedir(d);
   return n;
}

int main(void)
{
   char home[256], ws[300];
   snprintf(home, sizeof(home), "/tmp/aimee-mig-%d", (int)getpid());
   snprintf(ws, sizeof(ws), "%s/ws", home);
   run("rm -rf %s && mkdir -p %s", home, home);
   setenv("AIMEE_HOME", home, 1);
   setenv("AIMEE_WORKSPACES_DIR", ws, 1);

   /* A realistic legacy multi-tenant tree. */
   run("mkdir -p %s/webusers/alice/solo && echo alice-solo > %s/webusers/alice/solo/f", ws, ws);
   run("mkdir -p %s/webusers/alice/acme/orgproj && echo alice-org > "
       "%s/webusers/alice/acme/orgproj/f",
       ws, ws);
   run("mkdir -p %s/webusers/bob/bobonly && echo bob-only > %s/webusers/bob/bobonly/f", ws, ws);
   /* THE collision: both actors hold a project of the same name, with DIFFERENT
    * content. One of them cannot keep the plain name — neither may be lost. */
   run("mkdir -p %s/webusers/alice/shared && echo alice-shared > %s/webusers/alice/shared/f", ws,
       ws);
   run("mkdir -p %s/webusers/bob/shared && echo bob-shared > %s/webusers/bob/shared/f", ws, ws);
   /* A non-project stray file an editor might have left at the actor root. */
   run("echo stray > %s/webusers/alice/notes.txt", ws);

   char root[PATH_MAX];
   int rc = ws_scope_environment_root(root, sizeof(root));
   printf("ws_scope_environment_root rc=%d root=%s\n", rc, rc == 0 ? root : "(none)");
   ck(rc == 0, "environment root resolves");
   if (rc != 0)
   {
      printf("\nPASS=%d FAIL=%d\n", PASS, FAIL);
      return 1;
   }

   /* Every distinct piece of legacy content must still exist somewhere. */
   ck(found_marker(root, "alice-solo"), "alice/solo survived");
   ck(found_marker(root, "alice-org"), "alice/acme/orgproj survived (nested org)");
   ck(found_marker(root, "bob-only"), "bob/bobonly survived");
   ck(found_marker(root, "alice-shared"), "colliding alice/shared survived");
   ck(found_marker(root, "bob-shared"), "colliding bob/shared survived (NOT overwritten)");
   ck(found_marker(root, "stray"), "stray actor-root file survived");

   /* The plain name must be taken by exactly one of them, and the loser kept
    * under a distinct name — 6 top-level entries, none lost. */
   int n = count_entries(root);
   printf("  (environment root holds %d entries)\n", n);
   ck(n == 6, "all 6 legacy entries present at the environment root");

   /* Idempotent: a second resolve must not duplicate or destroy anything. */
   char root2[PATH_MAX];
   ck(ws_scope_environment_root(root2, sizeof(root2)) == 0 && strcmp(root, root2) == 0,
      "second resolve is stable");
   ck(count_entries(root) == n, "second resolve did not change the entry count");

   /* The legacy actor dirs should be gone (fully migrated), leaving no shadow
    * tree that a later resolve could migrate a second time. */
   char legacy[PATH_MAX];
   snprintf(legacy, sizeof(legacy), "%s/webusers/alice", ws);
   struct stat st;
   ck(stat(legacy, &st) != 0, "legacy alice dir removed after migration");
   snprintf(legacy, sizeof(legacy), "%s/webusers/bob", ws);
   ck(stat(legacy, &st) != 0, "legacy bob dir removed after migration");

   printf("\n--- environment root listing ---\n");
   run("ls -la %s", root);

   run("rm -rf %s", home);
   printf("\nPASS=%d FAIL=%d\n", PASS, FAIL);
   return FAIL == 0 ? 0 : 1;
}
