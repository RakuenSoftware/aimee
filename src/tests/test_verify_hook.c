/* test_verify_hook.c: unit tests for verify_install_git_hook() */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "modules/git/git_verify.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* ---- Helpers ---- */

static char g_tmpdir[256];

static void setup_fake_git_repo(void)
{
   snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/aimee-hook-test-XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_tmpdir) != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' init -q && git -C '%s' config user.email 'test@test' "
            "&& git -C '%s' config user.name 'test'",
            g_tmpdir, g_tmpdir, g_tmpdir);
   assert(system(cmd) == 0);
}

static void cleanup(void)
{
   if (g_tmpdir[0])
   {
      char cmd[512];
      snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
      (void)system(cmd);
      g_tmpdir[0] = '\0';
   }
}

static char *hook_path(void)
{
   static char buf[512];
   snprintf(buf, sizeof(buf), "%s/.git/hooks/pre-push", g_tmpdir);
   return buf;
}

static int file_exists(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0;
}

static int file_is_executable(const char *path)
{
   struct stat st;
   if (stat(path, &st) != 0)
      return 0;
   return (st.st_mode & S_IXUSR) != 0;
}

static int file_contains(const char *path, const char *needle)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   char buf[4096] = {0};
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   (void)n;
   return strstr(buf, needle) != NULL;
}

/* ---- Tests ---- */

static void test_install_creates_hook(void)
{
   setup_fake_git_repo();
   int rc = verify_install_git_hook(g_tmpdir);
   assert(rc == 0);
   assert(file_exists(hook_path()));
   assert(file_is_executable(hook_path()));
   assert(file_contains(hook_path(), "installed by aimee"));
   assert(file_contains(hook_path(), "aimee git verify action=check"));
   cleanup();
   printf("  PASS: test_install_creates_hook\n");
}

static void test_install_overwrites_own_hook(void)
{
   setup_fake_git_repo();
   /* Install once */
   assert(verify_install_git_hook(g_tmpdir) == 0);
   /* Install again — should succeed (idempotent) */
   int rc = verify_install_git_hook(g_tmpdir);
   assert(rc == 0);
   assert(file_contains(hook_path(), "installed by aimee"));
   cleanup();
   printf("  PASS: test_install_overwrites_own_hook\n");
}

static void test_install_skips_foreign_hook(void)
{
   setup_fake_git_repo();

   /* Write a hook that is not ours */
   char hpath[512];
   snprintf(hpath, sizeof(hpath), "%s/.git/hooks", g_tmpdir);
   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "mkdir -p '%s' && printf '#!/bin/sh\\necho custom\\n' > '%s/pre-push' && chmod +x "
            "'%s/pre-push'",
            hpath, hpath, hpath);
   assert(system(cmd) == 0);

   int rc = verify_install_git_hook(g_tmpdir);
   assert(rc == -2);
   /* Original content still intact */
   assert(file_contains(hook_path(), "custom"));
   assert(!file_contains(hook_path(), "installed by aimee"));
   cleanup();
   printf("  PASS: test_install_skips_foreign_hook\n");
}

static void test_install_fails_outside_git_repo(void)
{
   /* Use /tmp directly — not a git repo */
   char not_a_repo[] = "/tmp";
   int rc = verify_install_git_hook(not_a_repo);
   assert(rc == -1);
   printf("  PASS: test_install_fails_outside_git_repo\n");
}

/* ---- main ---- */

int main(void)
{
   printf("test_verify_hook\n");
   test_install_creates_hook();
   test_install_overwrites_own_hook();
   test_install_skips_foreign_hook();
   test_install_fails_outside_git_repo();
   printf("All tests passed.\n");
   return 0;
}
