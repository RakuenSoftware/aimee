/* Direct tests for the privileged legacy-Vault ownership repair. */
#include "server/vault_bootstrap_privilege.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static const char *g_config_home;

/* Minimal production dependency used by vault_bootstrap_run_as(). */
const char *config_default_dir(void)
{
   return g_config_home;
}

static void path_join(char *out, size_t cap, const char *base, const char *name)
{
   int n = snprintf(out, cap, "%s/%s", base, name);
   assert(n > 0 && (size_t)n < cap);
}

static void new_home(char home[128])
{
   snprintf(home, 128, "%s/aimee-vault-owner-XXXXXX", platform_tmpdir());
   assert(mkdtemp(home) != NULL);
}

static void make_vault(const char *home, char vault[160])
{
   path_join(vault, 160, home, ".vault");
   assert(mkdir(vault, 0755) == 0);
}

static void write_file(const char *path)
{
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
   assert(fd >= 0);
   assert(write(fd, "x", 1) == 1);
   assert(close(fd) == 0);
}

static void assert_mode_owner(const char *path, mode_t mode, uid_t uid, gid_t gid)
{
   struct stat st;
   assert(lstat(path, &st) == 0);
   assert((st.st_mode & 07777) == mode);
   assert(st.st_uid == uid);
   assert(st.st_gid == gid);
}

static void test_parse_args(void)
{
   const char *drop = (const char *)1;
   char *plain[] = {"aimee-server", "--bootstrap-vault-env", NULL};
   assert(vault_bootstrap_parse_args(2, plain, &drop) == 0 && drop == NULL);
   char *dropped[] = {"aimee-server", "--bootstrap-vault-env", "--drop-user", "aimee", NULL};
   assert(vault_bootstrap_parse_args(4, dropped, &drop) == 0 && strcmp(drop, "aimee") == 0);
   char *missing[] = {"aimee-server", "--bootstrap-vault-env", "--drop-user", NULL};
   assert(vault_bootstrap_parse_args(3, missing, &drop) == -1);
   char *empty[] = {"aimee-server", "--bootstrap-vault-env", "--drop-user", "", NULL};
   assert(vault_bootstrap_parse_args(4, empty, &drop) == -1);
   char *wrong[] = {"aimee-server", "--bootstrap-vault-env", "--user", "aimee", NULL};
   assert(vault_bootstrap_parse_args(4, wrong, &drop) == -1);
   puts("  PASS: bootstrap argv is closed and arity-safe");
}

static void test_missing_and_idempotent(void)
{
   char home[128], vault[160], file[192];
   new_home(home);
   assert(vault_bootstrap_repair_owner_at(home, geteuid(), getegid()) == 0);
   make_vault(home, vault);
   assert(vault_bootstrap_repair_owner_at(home, geteuid(), getegid()) == 0);
   assert_mode_owner(vault, 0700, geteuid(), getegid());
   path_join(file, sizeof(file), vault, "server.key");
   write_file(file);
   assert(chmod(vault, 0755) == 0);
   assert(vault_bootstrap_repair_owner_at(home, geteuid(), getegid()) == 0);
   assert_mode_owner(vault, 0700, geteuid(), getegid());
   assert_mode_owner(file, 0600, geteuid(), getegid());
   assert(vault_bootstrap_repair_owner_at(home, geteuid(), getegid()) == 0);
   assert(unlink(file) == 0 && rmdir(vault) == 0 && rmdir(home) == 0);
   puts("  PASS: absent Vault is a no-op; repair is mode-safe and idempotent");
}

static void test_missing_user_fails_closed(void)
{
   assert(vault_bootstrap_run_as("aimee-test-user-that-must-not-exist-7c42") == -1);
   puts("  PASS: nonexistent drop user fails closed");
}

static void test_symlinks_fail_closed(void)
{
   char home[128], vault[160], child[192], target[192];
   new_home(home);
   path_join(vault, sizeof(vault), home, ".vault");
   assert(symlink("/tmp", vault) == 0);
   assert(vault_bootstrap_repair_owner_at(home, geteuid(), getegid()) == -1);
   assert(unlink(vault) == 0);
   make_vault(home, vault);
   path_join(target, sizeof(target), home, "target");
   path_join(child, sizeof(child), vault, "link");
   write_file(target);
   assert(symlink(target, child) == 0);
   assert(vault_bootstrap_repair_owner_at(home, geteuid(), getegid()) == -1);
   assert(chmod(vault, 0700) == 0);
   assert(unlink(child) == 0 && unlink(target) == 0 && rmdir(vault) == 0 && rmdir(home) == 0);
   puts("  PASS: Vault and child symlinks fail closed");
}

static void test_nonregular_children_fail_without_blocking(void)
{
   char home[128], vault[160], child[192];
   new_home(home);
   make_vault(home, vault);
   path_join(child, sizeof(child), vault, "nested");
   assert(mkdir(child, 0700) == 0);
   assert(vault_bootstrap_repair_owner_at(home, geteuid(), getegid()) == -1);
   assert(chmod(vault, 0700) == 0);
   assert(rmdir(child) == 0);
   path_join(child, sizeof(child), vault, "pipe");
   assert(mkfifo(child, 0600) == 0);
   assert(vault_bootstrap_repair_owner_at(home, geteuid(), getegid()) == -1);
   assert(chmod(vault, 0700) == 0);
   assert(unlink(child) == 0 && rmdir(vault) == 0 && rmdir(home) == 0);
   puts("  PASS: directories and FIFOs fail closed without stalling bootstrap");
}

static void test_unexpected_owner_fails_closed(void)
{
   char home[128], vault[160];
   new_home(home);
   make_vault(home, vault);
   uid_t target = geteuid() + 1;
   if (geteuid() == 0)
   {
      /* Root is the one accepted legacy owner, so assign a third uid. */
      target = 1;
      assert(chown(vault, 2, 2) == 0);
   }
   assert(vault_bootstrap_repair_owner_at(home, target, getegid()) == -1);
   assert(rmdir(vault) == 0 && rmdir(home) == 0);
   puts("  PASS: unexpected ownership fails closed");
}

static void test_real_root_owner_repair_and_drop(void)
{
   if (geteuid() != 0)
   {
      puts("  SKIP: root-to-runtime chown/drop path (covered by container E2E)");
      return;
   }
   struct passwd *pw = getpwnam("nobody");
   if (!pw || pw->pw_uid == 0)
   {
      puts("  SKIP: root-to-runtime chown/drop path (no nobody account)");
      return;
   }
   char home[128], vault[160], file[192];
   new_home(home);
   assert(chmod(home, 0755) == 0);
   make_vault(home, vault);
   path_join(file, sizeof(file), vault, "server.key");
   write_file(file);
   assert(vault_bootstrap_repair_owner_at(home, pw->pw_uid, pw->pw_gid) == 0);
   assert_mode_owner(vault, 0700, pw->pw_uid, pw->pw_gid);
   assert_mode_owner(file, 0600, pw->pw_uid, pw->pw_gid);

   /* Recreate a root-owned fixture and verify the broker's post-condition in a
    * child: the parent must retain root only to clean the isolated fixture. */
   assert(chown(vault, 0, 0) == 0 && chown(file, 0, 0) == 0);
   g_config_home = home;
   pid_t pid = fork();
   assert(pid >= 0);
   if (pid == 0)
   {
      int rc = vault_bootstrap_run_as("nobody");
      _exit(rc == 0 && geteuid() == pw->pw_uid && getegid() == pw->pw_gid ? 0 : 1);
   }
   int status = 0;
   assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
   assert(unlink(file) == 0 && rmdir(vault) == 0 && rmdir(home) == 0);
   puts("  PASS: root-owned Vault repair precedes an irrevocable uid/gid drop");
}

static void test_multichild_validation_precedes_mutation(void)
{
   if (geteuid() != 0)
   {
      puts("  SKIP: multi-child foreign-owner rollback path (requires root fixture)");
      return;
   }
   struct passwd *pw = getpwnam("nobody");
   if (!pw || pw->pw_uid == 0 || pw->pw_uid == 2)
   {
      puts("  SKIP: multi-child foreign-owner rollback path (no suitable nobody account)");
      return;
   }

   char home[128], vault[160], first[192], second[192], foreign[192];
   new_home(home);
   make_vault(home, vault);
   path_join(first, sizeof(first), vault, "a-valid");
   path_join(second, sizeof(second), vault, "b-valid");
   path_join(foreign, sizeof(foreign), vault, "z-foreign");
   write_file(first);
   write_file(second);
   write_file(foreign);
   assert(chown(foreign, 2, 2) == 0);

   assert(vault_bootstrap_repair_owner_at(home, pw->pw_uid, pw->pw_gid) == -1);
   assert_mode_owner(first, 0644, 0, 0);
   assert_mode_owner(second, 0644, 0, 0);
   assert_mode_owner(foreign, 0644, 2, 2);
   assert_mode_owner(vault, 0500, 0, 0); /* unsafe directory stays quarantined */

   assert(unlink(first) == 0 && unlink(second) == 0 && unlink(foreign) == 0);
   assert(rmdir(vault) == 0 && rmdir(home) == 0);
   puts("  PASS: multi-child validation completes before mutation");
}

int main(void)
{
   test_parse_args();
   test_missing_and_idempotent();
   test_missing_user_fails_closed();
   test_symlinks_fail_closed();
   test_nonregular_children_fail_without_blocking();
   test_unexpected_owner_fails_closed();
   test_real_root_owner_repair_and_drop();
   test_multichild_validation_precedes_mutation();
   puts("vault bootstrap privilege tests: all passed");
   return 0;
}
