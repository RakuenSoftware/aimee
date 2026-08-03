/* test_forge_credentials_live.c — the forge-credential broker exercised against
 * a REAL authenticated git remote (workspace-resource-plane AC #3). Proves that
 * a brokered token clones + pushes through the broker's exec-env injection, that
 * the token never lands on disk, and that revoking it removes push access.
 *
 * Gated: skips (exit 0) unless the forge is configured via env:
 *   AIMEE_TEST_FORGE_REPO    e.g. http://aimee@192.168.0.101:3000/aimee/workspace-test.git
 *   AIMEE_TEST_FORGE_TOKEN   a write-scoped forge token
 *   AIMEE_TEST_FORGE_ASKPASS path to scripts/git-askpass-forge.sh
 * so `make unit-tests` / verify (no forge) never depend on it. Run live via
 * `make forge-cred-integration`. */
#define _GNU_SOURCE
#include "modules/git/forge_credentials.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* Run `argv` in `cwd` with environment `envp` (NULL → inherit). Returns the
 * child exit status, or -1 on spawn failure. */
static int run_git(const char *cwd, char *const argv[], char *const envp[])
{
   pid_t pid = fork();
   if (pid < 0)
      return -1;
   if (pid == 0)
   {
      if (cwd && chdir(cwd) != 0)
         _exit(127);
      execvpe(argv[0], argv, envp ? envp : environ);
      _exit(127);
   }
   int st = 0;
   if (waitpid(pid, &st, 0) < 0)
      return -1;
   return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Recursively grep `dir` for `needle`; returns 1 if found. */
static int tree_contains(const char *dir, const char *needle)
{
   char cmd[2048];
   snprintf(cmd, sizeof(cmd), "grep -rqaF -- '%s' '%s' 2>/dev/null", needle, dir);
   return system(cmd) == 0;
}

int main(void)
{
   const char *repo = getenv("AIMEE_TEST_FORGE_REPO");
   const char *token = getenv("AIMEE_TEST_FORGE_TOKEN");
   const char *askpass = getenv("AIMEE_TEST_FORGE_ASKPASS");
   if (!repo || !repo[0] || !token || !token[0] || !askpass || !askpass[0])
   {
      printf("forge_credentials_live: skipped (set AIMEE_TEST_FORGE_REPO / _TOKEN / _ASKPASS)\n");
      return 0;
   }

   long now = (long)time(NULL);
   char tmpl[] = "/tmp/forge_live.XXXXXX";
   char *base = mkdtemp(tmpl);
   assert(base != NULL);
   char repodir[512];
   snprintf(repodir, sizeof(repodir), "%s/repo", base);

   const char *WS = repodir; /* the workspace handle is its root path */

   /* install the brokered token for this workspace */
   assert(forge_cred_install(WS, token, "project", 300, now) == 0);

   /* clone THROUGH the broker env (GH_TOKEN + GIT_ASKPASS shim) — no token in
    * the URL, no token on the command line */
   char **env1 = forge_cred_build_env(WS, now, environ, askpass);
   assert(env1 != NULL);
   {
      char *argv[] = {(char *)"git",   (char *)"-c", (char *)"http.sslVerify=false",
                      (char *)"clone", (char *)"-q", (char *)repo,
                      (char *)repodir, NULL};
      int rc = run_git(NULL, argv, env1);
      assert(rc == 0); /* brokered clone succeeded */
   }

   /* make a commit */
   {
      char *cfg1[] = {(char *)"git", (char *)"config", (char *)"user.email", (char *)"t@t.io",
                      NULL};
      char *cfg2[] = {(char *)"git", (char *)"config", (char *)"user.name", (char *)"forge-test",
                      NULL};
      assert(run_git(repodir, cfg1, environ) == 0);
      assert(run_git(repodir, cfg2, environ) == 0);
      char fpath[600];
      snprintf(fpath, sizeof(fpath), "%s/brokered-%ld.txt", repodir, now);
      FILE *f = fopen(fpath, "w");
      assert(f);
      fprintf(f, "brokered push at %ld\n", now);
      fclose(f);
      char *add[] = {(char *)"git", (char *)"add", (char *)"-A", NULL};
      char *ci[] = {(char *)"git",
                    (char *)"commit",
                    (char *)"-q",
                    (char *)"-m",
                    (char *)"brokered token push",
                    NULL};
      assert(run_git(repodir, add, environ) == 0);
      assert(run_git(repodir, ci, environ) == 0);
   }

   /* PUSH through the broker env — the credential injection under test */
   {
      char *argv[] = {(char *)"git",  (char *)"-c", (char *)"http.sslVerify=false",
                      (char *)"push", (char *)"-q", (char *)"origin",
                      (char *)"HEAD", NULL};
      int rc = run_git(repodir, argv, env1);
      assert(rc == 0); /* brokered push succeeded */
   }
   forge_cred_free_env(env1);

   /* the token must NOT have leaked to disk anywhere under the checkout */
   assert(!tree_contains(repodir, token));

   /* REVOKE: the broker drops the token; build_env now yields nothing, and a
    * push with no credentials (terminal prompt disabled) must FAIL. */
   forge_cred_revoke(WS);
   assert(forge_cred_build_env(WS, now, environ, askpass) == NULL);
   {
      /* fresh commit to have something to push */
      char fpath[600];
      snprintf(fpath, sizeof(fpath), "%s/after-revoke.txt", repodir);
      FILE *f = fopen(fpath, "w");
      assert(f);
      fprintf(f, "should not reach the remote\n");
      fclose(f);
      char *add[] = {(char *)"git", (char *)"add", (char *)"-A", NULL};
      char *ci[] = {(char *)"git", (char *)"commit",      (char *)"-q",
                    (char *)"-m",  (char *)"post-revoke", NULL};
      assert(run_git(repodir, add, environ) == 0);
      assert(run_git(repodir, ci, environ) == 0);

      /* push with an env that has NO token and no interactive prompt */
      char *noauth[] = {(char *)"GIT_TERMINAL_PROMPT=0", (char *)"GIT_ASKPASS=/bin/false", NULL};
      char *argv[] = {(char *)"git",  (char *)"-c", (char *)"http.sslVerify=false",
                      (char *)"push", (char *)"-q", (char *)"origin",
                      (char *)"HEAD", NULL};
      int rc = run_git(repodir, argv, noauth);
      assert(rc != 0); /* revoked → push denied */
   }

   /* cleanup */
   {
      char cmd[600];
      snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
      (void)(system(cmd) + 1);
   }

   printf("forge_credentials_live: all tests passed (brokered clone+push, no disk leak, "
          "revoke denies)\n");
   return 0;
}
