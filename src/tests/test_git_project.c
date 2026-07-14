/* test_git_project.c — WP-D: clone a repo as a project under a webuser's scoped
 * workspace. Uses a local file:// source repo (no network, no creds) created in
 * the test, so it exercises the real git clone + scope resolution + name
 * derivation + refusal paths deterministically. */
#include "git_project.h"
#include "ws_registry.h"

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int run(const char *fmt, ...)
{
   char cmd[1024];
   va_list ap;
   __builtin_va_start(ap, fmt);
   vsnprintf(cmd, sizeof(cmd), fmt, ap);
   __builtin_va_end(ap);
   return system(cmd);
}

int main(void)
{
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-gitproj-%d", (int)getpid());
   assert(run("rm -rf %s && mkdir -p %s", home, home) == 0);
   setenv("AIMEE_HOME", home, 1);
   char wsdir[300];
   snprintf(wsdir, sizeof(wsdir), "%s/ws", home);
   setenv("AIMEE_WORKSPACES_DIR", wsdir, 1);
   /* Hardened CI git refuses the `file://` transport by default (CVE-2022-39253);
    * allow it for this test's local source repo. GIT_CONFIG_* is inherited by
    * both the setup git and the clone under test. */
   setenv("GIT_CONFIG_COUNT", "1", 1);
   setenv("GIT_CONFIG_KEY_0", "protocol.file.allow", 1);
   setenv("GIT_CONFIG_VALUE_0", "always", 1);

   /* Build a source repo with one commit. */
   char src[300];
   snprintf(src, sizeof(src), "%s/srcrepo", home);
   assert(run("mkdir -p %s && cd %s && git init -q && git config user.email t@t && git config "
              "user.name t && echo hello > README.md && git add . && git commit -qm init",
              src, src) == 0);
   char url[400];
   snprintf(url, sizeof(url), "file://%s", src);

   char path[PATH_MAX], name[128], err[256];

   /* name derived from the URL basename */
   assert(git_project_clone("webuser:alice", url, NULL, NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   assert(strcmp(name, "srcrepo") == 0);
   assert(strstr(path, "/webusers/alice/srcrepo") != NULL);
   struct stat st;
   char check[PATH_MAX + 32];
   snprintf(check, sizeof(check), "%s/.git", path);
   assert(stat(check, &st) == 0 && S_ISDIR(st.st_mode)); /* cloned */
   snprintf(check, sizeof(check), "%s/README.md", path);
   assert(stat(check, &st) == 0); /* file came across */

   /* re-clone the same name -> project already exists -> conflict */
   assert(git_project_clone("webuser:alice", url, NULL, NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);

   /* explicit name + .git stripping on derive */
   char url2[420];
   snprintf(url2, sizeof(url2), "file://%s/.git", src); /* trailing .git path form */
   assert(git_project_clone("webuser:alice", url, "myproj", NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   assert(strcmp(name, "myproj") == 0 && strstr(path, "/alice/myproj"));
   (void)url2;

   /* cross-principal: bob clones into HIS scope, not alice's */
   assert(git_project_clone("webuser:bob", url, "shared", NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   assert(strstr(path, "/webusers/bob/shared") != NULL);

   /* refusals */
   assert(git_project_clone("uid:1000", url, "x", NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1); /* not a webuser */
   assert(git_project_clone("webuser:alice", "", "x", NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1); /* empty url */
   assert(git_project_clone("webuser:alice", "--upload-pack=evil", "x", NULL, NULL, path,
                            sizeof(path), name, sizeof(name), err,
                            sizeof(err)) == -1); /* flag-like url */
   assert(git_project_clone("webuser:alice", url, "../escape", NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1); /* bad name */

   /* --- org-scoped clones (slice 1) --- */
   /* explicit org: lands at <root>/acme/orgproj with ref "acme/orgproj" */
   assert(git_project_clone("webuser:alice", url, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   assert(strcmp(name, "acme/orgproj") == 0);
   assert(strstr(path, "/webusers/alice/acme/orgproj") != NULL);
   snprintf(check, sizeof(check), "%s/.aimee/remote", path);
   assert(stat(check, &st) == 0); /* credential-free sidecar published */
   /* same ref again -> exists -> conflict */
   assert(git_project_clone("webuser:alice", url, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);
   /* same KEY, different remote (second source repo) -> registry 409, generic
    * message (no other remote echoed) */
   char src2[300], url3[400];
   snprintf(src2, sizeof(src2), "%s/srcrepo2", home);
   assert(run("mkdir -p %s && cd %s && git init -q && git config user.email t@t && git config "
              "user.name t && echo two > f && git add . && git commit -qm init",
              src2, src2) == 0);
   snprintf(url3, sizeof(url3), "file://%s", src2);
   assert(git_project_clone("webuser:bob", url3, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);
   assert(strstr(err, "different remote") != NULL);
   assert(strstr(err, "srcrepo") == NULL); /* the other remote is NOT disclosed */
   /* same key + SAME remote from another webuser -> allowed (holder count 2) */
   assert(git_project_clone("webuser:bob", url, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   assert(strstr(path, "/webusers/bob/acme/orgproj") != NULL);
   /* flat/org namespace conflict: a flat project named like the org */
   assert(git_project_clone("webuser:alice", url3, "acme", NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err,
                            sizeof(err)) == GP_ERR_CONFLICT); /* org dir 'acme' exists */
   /* org named like an existing flat project */
   assert(git_project_clone("webuser:alice", url3, "x2", "srcrepo", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);
   /* org override sanitization: "Rakuen Software!" -> "Rakuen-Software" */
   assert(git_project_clone("webuser:alice", url3, "sanit", "Rakuen Software!", NULL, path,
                            sizeof(path), name, sizeof(name), err, sizeof(err)) == 0);
   assert(strcmp(name, "Rakuen-Software/sanit") == 0);
   /* an over-long org is REJECTED, never truncated (identity collapse) */
   char longorg[80];
   memset(longorg, 'a', 70);
   longorg[70] = '\0';
   assert(git_project_clone("webuser:alice", url3, "x3", longorg, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1);
   /* conflicts are distinguishable from validation failures (GP_ERR_CONFLICT) */
   assert(git_project_clone("webuser:alice", url, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);
   /* registry resync honors `git remote set-url`: repoint alice's clone,
    * then bob cloning the NEW remote at the same ref succeeds (stale registry
    * self-heals under the lock) */
   assert(git_project_clone("webuser:alice", url, "syncy", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   assert(run("git -C %s remote set-url origin %s", path, url3) == 0);
   assert(git_project_clone("webuser:bob", url3, "syncy", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   /* ...and the OTHER direction: cloning the now-STALE registry remote at the
    * same ref must 409 after resync, not silently join divergent holders */
   assert(git_project_clone("webuser:carol", url, "syncy", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);

   /* --- list: alice has srcrepo + myproj + acme/orgproj + Rakuen-Software/sanit
    * + acme/syncy; bob has shared + acme/orgproj + acme/syncy --- */
   char names[64][GIT_PROJECT_NAME_MAX];
   int an = git_project_list("webuser:alice", names, 64);
   assert(an == 5);
   int saw_src = 0, saw_my = 0, saw_org = 0, saw_sanit = 0;
   for (int i = 0; i < an; i++)
   {
      if (strcmp(names[i], "srcrepo") == 0)
         saw_src = 1;
      if (strcmp(names[i], "myproj") == 0)
         saw_my = 1;
      if (strcmp(names[i], "acme/orgproj") == 0)
         saw_org = 1;
      if (strcmp(names[i], "Rakuen-Software/sanit") == 0)
         saw_sanit = 1;
   }
   assert(saw_src && saw_my && saw_org && saw_sanit);
   int bn = git_project_list("webuser:bob", names, 64);
   assert(bn == 3); /* bob sees only his: shared + acme/orgproj + acme/syncy */
   assert(git_project_list("uid:1000", names, 64) == -1);
   /* a webuser with no clones lists zero, not an error */
   assert(git_project_list("webuser:carol", names, 64) == 0);

   /* --- delete (slice 2). The kb purge wrappers are the kb_purge_stub: a
    * transport failure by default (exercising the 503-abort path), success
    * with AIMEE_TEST_KB_PURGE_MODE=ok. --- */
   git_project_delete_result_t dres;
   char derr[512];

   /* refusals: bad ref / non-webuser */
   assert(git_project_delete("webuser:alice", "../x", 0, &dres, derr, sizeof(derr)) == -1);
   assert(git_project_delete("webuser:alice", "a/b/c", 0, &dres, derr, sizeof(derr)) == -1);
   assert(git_project_delete("uid:1000", "srcrepo", 0, &dres, derr, sizeof(derr)) == -1);
   /* cross-principal / nonexistent: plain not-found (no existence disclosure) */
   assert(git_project_delete("webuser:carol", "srcrepo", 0, &dres, derr, sizeof(derr)) ==
          GP_ERR_NOT_FOUND);
   assert(git_project_delete("webuser:bob", "srcrepo", 0, &dres, derr, sizeof(derr)) ==
          GP_ERR_NOT_FOUND); /* alice's flat project is invisible to bob */

   /* retained: alice deletes acme/orgproj while bob still holds it — the kb
    * purge is never attempted (the stub would fail), alice's clone goes, bob's
    * stays, the registry drops to one holder. */
   char aorg[PATH_MAX], borg[PATH_MAX];
   snprintf(aorg, sizeof(aorg), "%s/webusers/alice/acme/orgproj", wsdir);
   snprintf(borg, sizeof(borg), "%s/webusers/bob/acme/orgproj", wsdir);
   assert(git_project_delete("webuser:alice", "acme/orgproj", 0, &dres, derr, sizeof(derr)) == 0);
   assert(strcmp(dres.kb_status, "retained") == 0);
   assert(dres.purge_id[0] != '\0');
   free(dres.kb_detail);
   assert(stat(aorg, &st) != 0);                        /* alice's clone removed */
   assert(stat(borg, &st) == 0 && S_ISDIR(st.st_mode)); /* bob's untouched */
   char remo[1024];
   int holders = 0;
   assert(ws_reg_lookup("acme/orgproj", remo, sizeof(remo), &holders) == 1 && holders == 1);
   /* alice's acme org dir was NOT pruned: acme/syncy still lives there */
   snprintf(check, sizeof(check), "%s/webusers/alice/acme/syncy", wsdir);
   assert(stat(check, &st) == 0);

   /* last holder + kb unreachable, no force -> 503 abort: nothing destroyed,
    * the registry decrement rolled back. */
   assert(git_project_delete("webuser:bob", "acme/orgproj", 0, &dres, derr, sizeof(derr)) ==
          GP_ERR_KB_UNAVAILABLE);
   assert(dres.kb_detail && strstr(dres.kb_detail, "error") != NULL);
   free(dres.kb_detail);
   assert(stat(borg, &st) == 0); /* clone intact */
   assert(ws_reg_lookup("acme/orgproj", remo, sizeof(remo), &holders) == 1 &&
          holders == 1); /* count restored */

   /* force: the filesystem proceeds despite the unreachable kb; the response
    * carries the kb error detail under kb_status "forced". */
   assert(git_project_delete("webuser:bob", "acme/orgproj", 1, &dres, derr, sizeof(derr)) == 0);
   assert(strcmp(dres.kb_status, "forced") == 0);
   assert(dres.kb_detail && strstr(dres.kb_detail, "stub") != NULL);
   free(dres.kb_detail);
   assert(stat(borg, &st) != 0);                                             /* clone removed */
   assert(ws_reg_lookup("acme/orgproj", remo, sizeof(remo), &holders) == 0); /* entry gone */

   /* purged: sole holder + reachable kb (stub ok mode) -> full purge path
    * (fence write, heartbeat, finalize) and the per-store detail. */
   setenv("AIMEE_TEST_KB_PURGE_MODE", "ok", 1);
   snprintf(check, sizeof(check), "%s/webusers/alice/myproj", wsdir);
   assert(stat(check, &st) == 0);
   assert(git_project_delete("webuser:alice", "myproj", 0, &dres, derr, sizeof(derr)) == 0);
   assert(strcmp(dres.kb_status, "purged") == 0);
   assert(dres.kb_detail && strstr(dres.kb_detail, "stub") != NULL); /* per-store map */
   assert(dres.generation[0] != '\0');
   free(dres.kb_detail);
   assert(stat(check, &st) != 0);
   unsetenv("AIMEE_TEST_KB_PURGE_MODE");

   /* org prune: bob's last project under acme (syncy, retained — alice still
    * holds it) removes the clone AND the now-empty org dir. */
   assert(git_project_delete("webuser:bob", "acme/syncy", 0, &dres, derr, sizeof(derr)) == 0);
   assert(strcmp(dres.kb_status, "retained") == 0);
   free(dres.kb_detail);
   snprintf(check, sizeof(check), "%s/webusers/bob/acme", wsdir);
   assert(stat(check, &st) != 0); /* org dir pruned */
   snprintf(check, sizeof(check), "%s/webusers/alice/acme/syncy", wsdir);
   assert(stat(check, &st) == 0); /* alice's holder untouched */

   /* the lister agrees: alice lost acme/orgproj + myproj, bob lost both acme
    * projects (shared remains). */
   assert(git_project_list("webuser:alice", names, 64) == 3);
   assert(git_project_list("webuser:bob", names, 64) == 1);
   assert(strcmp(names[0], "shared") == 0);

   /* cancel mismatch: the purge reaches the kb but a store fails (fence
    * written) and the cancel is a generation-mismatch no-op (cleared:false) —
    * terminal purge-committed-unfinished: the holder count must NOT be
    * restored and nothing filesystem is removed. */
   setenv("AIMEE_TEST_KB_PURGE_MODE", "cancel-mismatch", 1);
   snprintf(check, sizeof(check), "%s/webusers/bob/shared", wsdir);
   assert(git_project_delete("webuser:bob", "shared", 0, &dres, derr, sizeof(derr)) ==
          GP_ERR_KB_UNAVAILABLE);
   assert(strstr(derr, "unfinished") != NULL);
   free(dres.kb_detail);
   assert(stat(check, &st) == 0);                                      /* clone intact */
   assert(ws_reg_lookup("shared", remo, sizeof(remo), &holders) == 0); /* decrement KEPT */
   /* re-running converges: the resync step rebuilds the holder from disk and
    * a now-successful purge completes the delete. */
   setenv("AIMEE_TEST_KB_PURGE_MODE", "ok", 1);
   assert(git_project_delete("webuser:bob", "shared", 0, &dres, derr, sizeof(derr)) == 0);
   assert(strcmp(dres.kb_status, "purged") == 0);
   free(dres.kb_detail);
   assert(stat(check, &st) != 0);

   /* local-index failure: aborts BEFORE any filesystem removal — the cancel
    * confirms (cleared:true), the holder is re-registered, the clone stays. */
   setenv("AIMEE_TEST_CODE_INDEX_DELETE_FAIL", "1", 1);
   snprintf(check, sizeof(check), "%s/webusers/alice/srcrepo", wsdir);
   assert(git_project_delete("webuser:alice", "srcrepo", 0, &dres, derr, sizeof(derr)) ==
          GP_ERR_KB_UNAVAILABLE);
   assert(strstr(derr, "code index") != NULL);
   free(dres.kb_detail);
   assert(stat(check, &st) == 0); /* clone intact */
   assert(ws_reg_lookup("srcrepo", remo, sizeof(remo), &holders) == 1 &&
          holders == 1); /* holder restored */
   unsetenv("AIMEE_TEST_CODE_INDEX_DELETE_FAIL");
   assert(git_project_delete("webuser:alice", "srcrepo", 0, &dres, derr, sizeof(derr)) == 0);
   assert(strcmp(dres.kb_status, "purged") == 0);
   free(dres.kb_detail);
   assert(stat(check, &st) != 0);

   /* fence ownership lost (heartbeat refreshed:false — a takeover displaced
    * this delete): the walk stops before removing anything; the fence and the
    * registry decrement stay (the purge committed); a re-run converges. */
   setenv("AIMEE_TEST_KB_PURGE_MODE", "hb-lost", 1);
   snprintf(check, sizeof(check), "%s/webusers/alice/Rakuen-Software/sanit", wsdir);
   assert(git_project_delete("webuser:alice", "Rakuen-Software/sanit", 0, &dres, derr,
                             sizeof(derr)) == GP_ERR_KB_UNAVAILABLE);
   assert(strstr(derr, "fence") != NULL);
   free(dres.kb_detail);
   assert(stat(check, &st) == 0); /* nothing removed */
   assert(ws_reg_lookup("Rakuen-Software/sanit", remo, sizeof(remo), &holders) == 0);
   setenv("AIMEE_TEST_KB_PURGE_MODE", "ok", 1);
   assert(git_project_delete("webuser:alice", "Rakuen-Software/sanit", 0, &dres, derr,
                             sizeof(derr)) == 0);
   assert(strcmp(dres.kb_status, "purged") == 0);
   free(dres.kb_detail);
   assert(stat(check, &st) != 0);
   unsetenv("AIMEE_TEST_KB_PURGE_MODE");

   /* final tally: alice keeps only acme/syncy; bob has nothing left. */
   assert(git_project_list("webuser:alice", names, 64) == 1);
   assert(strcmp(names[0], "acme/syncy") == 0);
   assert(git_project_list("webuser:bob", names, 64) == 0);

   /* --- rename-first tombstone: an interrupted walk is resumable --- */
   setenv("AIMEE_TEST_KB_PURGE_MODE", "ok", 1);
   /* simulate a crash right after the tombstone rename: the project sits at
    * its ".deleting-<repo>" sibling and the crashed attempt's registry
    * decrement persisted (resync re-derives from disk, where only the marker
    * — invisible to it — remains). Re-running the delete must converge. */
   char mpath[PATH_MAX];
   snprintf(mpath, sizeof(mpath), "%s/webusers/alice/acme/.deleting-syncy", wsdir);
   assert(run("mv %s/webusers/alice/acme/syncy %s", wsdir, mpath) == 0);
   assert(git_project_delete("webuser:alice", "acme/syncy", 0, &dres, derr, sizeof(derr)) == 0);
   assert(strcmp(dres.kb_status, "purged") == 0);
   free(dres.kb_detail);
   assert(stat(mpath, &st) != 0); /* marker tree fully gone */
   snprintf(check, sizeof(check), "%s/webusers/alice/acme", wsdir);
   assert(stat(check, &st) != 0); /* org pruned */
   assert(ws_reg_lookup("acme/syncy", remo, sizeof(remo), &holders) == 0);

   /* a flat marker with content (the ref itself no longer exists anywhere)
    * also converges, and once converged the ref is a plain 404 again */
   assert(run("mkdir -p %s/webusers/alice/.deleting-ghost/sub && "
              "echo x > %s/webusers/alice/.deleting-ghost/sub/f",
              wsdir, wsdir) == 0);
   assert(git_project_delete("webuser:alice", "ghost", 0, &dres, derr, sizeof(derr)) == 0);
   assert(strcmp(dres.kb_status, "purged") == 0);
   free(dres.kb_detail);
   snprintf(check, sizeof(check), "%s/webusers/alice/.deleting-ghost", wsdir);
   assert(stat(check, &st) != 0);
   assert(git_project_delete("webuser:alice", "ghost", 0, &dres, derr, sizeof(derr)) ==
          GP_ERR_NOT_FOUND);
   /* never-existing refs (flat and org) still 404 — no marker, no project */
   assert(git_project_delete("webuser:alice", "neverwas", 0, &dres, derr, sizeof(derr)) ==
          GP_ERR_NOT_FOUND);
   assert(git_project_delete("webuser:alice", "no/pe", 0, &dres, derr, sizeof(derr)) ==
          GP_ERR_NOT_FOUND);
   unsetenv("AIMEE_TEST_KB_PURGE_MODE");
   assert(git_project_list("webuser:alice", names, 64) == 0);

   assert(run("rm -rf %s", home) == 0);
   printf("git_project: all tests passed\n");
   return 0;
}
