/* test_cli_remote.c: persisted remote-server config round-trip + precedence. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee_client.h"
#include "cli_remote.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define PASS(name) printf("  PASS: %s\n", name)

/* Filled in main() before mkdtemp: the template has to be built at RUNTIME now
 * that it comes from platform_tmpdir(), and a file-scope initializer cannot
 * call anything. */
static char g_home[256];

static void reset_state(void)
{
   aimee_client_set_remote(NULL, NULL);
   unsetenv("AIMEE_SERVER_URL");
   unsetenv("AIMEE_SERVER_TOKEN");
}

static void test_help_arity_and_help_token_handling(void)
{
   reset_state();
   char path[256];
   snprintf(path, sizeof(path), "%s/remote.conf", g_home);
   unlink(path);

   char *set_help[] = {(char *)"set", (char *)"--help"};
   assert(cli_remote_cmd(2, set_help, 1) == 0);
   assert(access(path, F_OK) != 0);

   char *status_help[] = {(char *)"status", (char *)"-h"};
   assert(cli_remote_cmd(2, status_help, 1) == 0);
   assert(access(path, F_OK) != 0);

   char *too_many[] = {(char *)"set", (char *)"http://example.test:8740", (char *)"tok",
                       (char *)"unexpected"};
   assert(cli_remote_cmd(4, too_many, 1) == 2);
   assert(access(path, F_OK) != 0);

   char *token_named_help[] = {(char *)"set", (char *)"http://example.test:8740", (char *)"help"};
   assert(cli_remote_cmd(3, token_named_help, 1) == 0);
   assert(access(path, F_OK) == 0);
   cli_remote_load_persisted();
   char desc[128] = {0};
   char token[128] = {0};
   assert(aimee_client_remote_active(desc, sizeof(desc)) == 1);
   assert(strcmp(desc, "example.test:8740") == 0);
   assert(aimee_client_remote_token(token, sizeof(token)) == 1);
   assert(strcmp(token, "help") == 0);
   unlink(path);
   reset_state();
   PASS("remote help/arity is side-effect free and a token named help is preserved");
}

static void test_set_then_load(void)
{
   reset_state();
   char *url = (char *)"http://example.test:8390";
   char *tok = (char *)"tok123";
   char *argv[] = {(char *)"set", url, tok};
   assert(cli_remote_cmd(3, argv, 1) == 0);

   char path[256];
   struct stat st;
   snprintf(path, sizeof(path), "%s/remote.conf", g_home);
   assert(stat(path, &st) == 0);
   assert((st.st_mode & 0777) == 0600);

   /* Re-setting a target must repair an older, permissive config rather than
    * preserving its mode through O_CREAT. */
   assert(chmod(path, 0644) == 0);
   assert(cli_remote_cmd(3, argv, 1) == 0);
   assert(stat(path, &st) == 0);
   assert((st.st_mode & 0777) == 0600);
   PASS("remote.conf is created private and permissive files are repaired");

   /* Persisted config drives the transport once loaded. */
   cli_remote_load_persisted();
   char desc[128] = {0};
   assert(aimee_client_remote_active(desc, sizeof(desc)) == 1);
   assert(strcmp(desc, "example.test:8390") == 0);
   PASS("set persists and load_persisted applies it");
}

static void test_env_wins_over_file(void)
{
   reset_state();
   /* File still present from previous test; env must take precedence. */
   setenv("AIMEE_SERVER_URL", "http://env-host:9000", 1);
   cli_remote_load_persisted(); /* must NOT override the active env target */
   char desc[128] = {0};
   assert(aimee_client_remote_active(desc, sizeof(desc)) == 1);
   assert(strcmp(desc, "env-host:9000") == 0);
   PASS("env AIMEE_SERVER_URL beats persisted remote.conf");
}

static void test_clear(void)
{
   reset_state();
   char *argv[] = {(char *)"clear"};
   assert(cli_remote_cmd(1, argv, 1) == 0);
   cli_remote_load_persisted();
   assert(aimee_client_remote_active(NULL, 0) == 0); /* nothing left to load */
   PASS("clear removes persisted config");
}

static void test_set_creates_missing_home(void)
{
   reset_state();
   /* The README's recommended deployment installs ONLY the thin client, so
    * nothing creates <aimee_home> (~/.config/aimee) before the user runs
    * `aimee remote set`. Point AIMEE_HOME at a not-yet-existing nested path and
    * confirm set creates it instead of failing with ENOENT (regression). */
   const char *saved = getenv("AIMEE_HOME");
   char nested[320];
   snprintf(nested, sizeof(nested), "%s/missing/aimee_home", saved ? saved : "/tmp");
   setenv("AIMEE_HOME", nested, 1);

   char *argv[] = {(char *)"set", (char *)"http://made-host:8740", (char *)"tok"};
   assert(cli_remote_cmd(3, argv, 1) == 0);
   cli_remote_load_persisted();
   char desc[128] = {0};
   assert(aimee_client_remote_active(desc, sizeof(desc)) == 1);
   assert(strcmp(desc, "made-host:8740") == 0);
   PASS("remote set creates a missing aimee_home dir");

   /* Best-effort cleanup + restore home for any later tests. */
   char path[400];
   snprintf(path, sizeof(path), "%s/remote.conf", nested);
   unlink(path);
   rmdir(nested);
   if (saved)
      setenv("AIMEE_HOME", saved, 1);
}

static void test_https_set_restores_insecure_env(void)
{
   reset_state();
   /* `remote set` forces strict verification (suspends AIMEE_TLS_INSECURE) so a
    * self-signed server is always pinned even when the env var is set. It must
    * RESTORE the caller's env afterward — never permanently clobber it. Use an
    * unresolvable https host: trust can't be established, so setup must report
    * failure even though the target remains persisted for a later `remote trust`.
    * The env var must still come back. (Regression for the auto-pin fix.) */
   setenv("AIMEE_TLS_INSECURE", "1", 1);
   char *argv[] = {(char *)"set", (char *)"https://made-host.invalid:8799", (char *)"tok"};
   assert(cli_remote_cmd(3, argv, 1) == 1);
   int is_https = 0;
   assert(aimee_client_remote_active_scheme(NULL, 0, &is_https) == 1);
   assert(is_https == 1); /* scheme reporting must not depend on requesting a description */
   const char *v = getenv("AIMEE_TLS_INSECURE");
   assert(v && strcmp(v, "1") == 0); /* restored, not clobbered */
   PASS("https remote set restores AIMEE_TLS_INSECURE");
   unsetenv("AIMEE_TLS_INSECURE");
}

int main(void)
{
   /* Isolate AIMEE_HOME so we never touch the real config. */
   snprintf(g_home, sizeof g_home, "%s/aimee_test_remote_XXXXXX", platform_tmpdir());
   char *dir = mkdtemp(g_home);
   assert(dir != NULL);
   setenv("AIMEE_HOME", dir, 1);

   printf("test_cli_remote:\n");
   test_help_arity_and_help_token_handling();
   test_set_then_load();
   test_env_wins_over_file();
   test_clear();
   test_set_creates_missing_home();
   test_https_set_restores_insecure_env();
   printf("ALL PASS\n");

   /* Best-effort cleanup. */
   char path[256];
   snprintf(path, sizeof(path), "%s/remote.conf", dir);
   unlink(path);
   rmdir(dir);
   return 0;
}
