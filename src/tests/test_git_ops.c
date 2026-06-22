/* test_git_ops.c — WP-E: per-project git operations, scoped + sanitized.
 * Builds a real repo (+ a local bare remote) under a webuser's scope and drives
 * status/log/branch/diff/commit/checkout/push/pull, plus the refusal paths. */
#include "git_ops.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int run(const char *fmt, ...)
{
   char cmd[2048];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(cmd, sizeof(cmd), fmt, ap);
   va_end(ap);
   return system(cmd);
}

static int out_has(const char *out, const char *needle)
{
   return out && strstr(out, needle) != NULL;
}

/* Fake per-session worktree resolver: maps <cwd> -> <cwd>/wt-<sid>. */
static int fake_isolation(const char *cwd, const char *sid, char *out, size_t out_len, int create)
{
   (void)create;
   if (!cwd || !sid || !sid[0])
      return 0;
   snprintf(out, out_len, "%s/wt-%s", cwd, sid);
   return 1;
}

int main(void)
{
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-gitops-%d", (int)getpid());
   assert(run("rm -rf %s && mkdir -p %s", home, home) == 0);
   setenv("AIMEE_HOME", home, 1);
   char ws[300];
   snprintf(ws, sizeof(ws), "%s/ws", home);
   setenv("AIMEE_WORKSPACES_DIR", ws, 1);
   setenv("GIT_CONFIG_COUNT", "1", 1);
   setenv("GIT_CONFIG_KEY_0", "protocol.file.allow", 1);
   setenv("GIT_CONFIG_VALUE_0", "always", 1);

   /* alice's project dir + a bare remote it tracks. */
   char proj[400], bare[400];
   snprintf(proj, sizeof(proj), "%s/webusers/alice/proj", ws);
   snprintf(bare, sizeof(bare), "%s/remote.git", home);
   assert(run("git init -q --bare %s", bare) == 0);
   assert(run("mkdir -p %s && cd %s && git init -q -b main && git config user.email t@t && git "
              "config user.name t && echo one > f.txt && git add . && git commit -qm init && git "
              "remote add origin file://%s && git push -qu origin main",
              proj, proj, bare) == 0);

   char *out = NULL;
   char err[256];

   /* status */
   assert(git_ops_run("webuser:alice", "proj", "status", NULL, 0, &out, err, sizeof(err)) == 0);
   assert(out_has(out, "main"));
   free(out);

   /* log shows the init commit */
   assert(git_ops_run("webuser:alice", "proj", "log", NULL, 5, &out, err, sizeof(err)) == 0);
   assert(out_has(out, "init"));
   free(out);

   /* branch lists main */
   assert(git_ops_run("webuser:alice", "proj", "branch", NULL, 0, &out, err, sizeof(err)) == 0);
   assert(out_has(out, "main"));
   free(out);

   /* commit a change, then log shows it */
   assert(run("cd %s && echo two >> f.txt", proj) == 0);
   assert(git_ops_run("webuser:alice", "proj", "commit", "second change", 0, &out, err,
                      sizeof(err)) == 0);
   free(out);
   assert(git_ops_run("webuser:alice", "proj", "log", NULL, 5, &out, err, sizeof(err)) == 0);
   assert(out_has(out, "second change"));
   free(out);

   /* diff after staging is empty; create an unstaged change and diff sees it */
   assert(run("cd %s && echo three >> f.txt", proj) == 0);
   assert(git_ops_run("webuser:alice", "proj", "diff", NULL, 0, &out, err, sizeof(err)) == 0);
   assert(out_has(out, "three"));
   free(out);

   /* push the committed change to the bare remote */
   assert(git_ops_run("webuser:alice", "proj", "push", NULL, 0, &out, err, sizeof(err)) == 0);
   free(out);
   /* fetch + pull are no-ops but must succeed */
   assert(git_ops_run("webuser:alice", "proj", "fetch", NULL, 0, &out, err, sizeof(err)) == 0);
   free(out);
   assert(git_ops_run("webuser:alice", "proj", "pull", NULL, 0, &out, err, sizeof(err)) == 0);
   free(out);

   /* checkout an existing branch */
   assert(run("cd %s && git branch -q feature", proj) == 0);
   assert(git_ops_run("webuser:alice", "proj", "checkout", "feature", 0, &out, err, sizeof(err)) ==
          0);
   free(out);

   /* --- refusals --- */
   assert(git_ops_run("uid:1000", "proj", "status", NULL, 0, &out, err, sizeof(err)) == -1);
   assert(git_ops_run("webuser:alice", "nope", "status", NULL, 0, &out, err, sizeof(err)) == -1);
   assert(git_ops_run("webuser:alice", "proj", "rm -rf /", NULL, 0, &out, err, sizeof(err)) == -1);
   assert(git_ops_run("webuser:alice", "proj", "checkout", "-evil", 0, &out, err, sizeof(err)) ==
          -1);
   assert(git_ops_run("webuser:alice", "proj", "commit", "", 0, &out, err, sizeof(err)) == -1);
   assert(git_ops_run("webuser:alice", "../escape", "status", NULL, 0, &out, err, sizeof(err)) ==
          -1);
   /* bob cannot touch alice's project (no such project in bob's scope) */
   assert(git_ops_run("webuser:bob", "proj", "status", NULL, 0, &out, err, sizeof(err)) == -1);

   /* --- session worktree redirect (git_ops_session_dir wiring) --- */
   char dir[4096];
   /* no session id -> the project checkout */
   assert(git_ops_session_dir("webuser:alice", "proj", NULL, dir, sizeof(dir), err, sizeof(err)) ==
          0);
   assert(strcmp(dir, proj) == 0);
   /* session id but no resolver registered -> still the project checkout */
   assert(git_ops_session_dir("webuser:alice", "proj", "sid-xyz", dir, sizeof(dir), err,
                              sizeof(err)) == 0);
   assert(strcmp(dir, proj) == 0);
   /* with a resolver: a non-empty session id redirects to its worktree */
   git_ops_register_session_isolation(fake_isolation);
   assert(git_ops_session_dir("webuser:alice", "proj", "sid-xyz", dir, sizeof(dir), err,
                              sizeof(err)) == 0);
   char expect[4200];
   snprintf(expect, sizeof(expect), "%s/wt-sid-xyz", proj);
   assert(strcmp(dir, expect) == 0);
   /* empty session id -> base even with a resolver registered */
   assert(git_ops_session_dir("webuser:alice", "proj", "", dir, sizeof(dir), err, sizeof(err)) ==
          0);
   assert(strcmp(dir, proj) == 0);
   /* refusal: a non-webuser principal is rejected before any resolution */
   assert(git_ops_session_dir("uid:1000", "proj", "sid-xyz", dir, sizeof(dir), err, sizeof(err)) ==
          -1);
   git_ops_register_session_isolation(NULL);

   assert(run("rm -rf %s", home) == 0);
   printf("git_ops: all tests passed\n");
   return 0;
}
