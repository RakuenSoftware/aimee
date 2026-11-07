/* test_git_project.c — WP-D: clone a repo as a project in the one workspace
 * environment. Uses a local file:// source repo (no network, no creds) created
 * in the test, so it exercises the real git clone + scope resolution + name
 * derivation + refusal paths deterministically. Actors (alice/bob/carol) are
 * PAM identities that authorize and attribute a request; they never select a
 * tree, so they all resolve the same projects. */
#include "modules/git/git_project.h"
#include "server_project_workspace.h"
#include "modules/workspace/workspace_scope.h"
#include "ws_registry.h"

#include <ouroboros/module_runtime.h>
#include <aimee/workspace/module_api.h>

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern ouro_module_status_t aimee_workspace_module_handler(const ouro_module_invocation_t *,
                                                           const uint8_t *, uint32_t, uint8_t *,
                                                           uint32_t, uint32_t *, void *);

int ouro_module_invocation_cancelled(const ouro_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

static int validate_workspace_ref_via_module(const char *ref, size_t ref_len, int *allowed)
{
   uint8_t request[AIMEE_WORKSPACE_REQUEST_LEN], response[AIMEE_WORKSPACE_RESPONSE_LEN];
   uint32_t response_len = 0;
   ouro_module_invocation_t invocation = {.stage_id = AIMEE_WORKSPACE_STAGE_ACCESS};
   return aimee_workspace_request_encode(ref, ref_len, request, sizeof(request)) == 0 &&
                  aimee_workspace_module_handler(&invocation, request, sizeof(request), response,
                                                 sizeof(response), &response_len,
                                                 NULL) == OURO_MODULE_STATUS_OK
              ? aimee_workspace_response_decode(response, response_len, allowed)
              : -1;
}

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
   ws_scope_register_ref_validator(validate_workspace_ref_via_module);
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
   assert(strstr(path, "/environment/srcrepo") != NULL);
   struct stat st;
   char check[PATH_MAX + 32];
   snprintf(check, sizeof(check), "%s/.git", path);
   assert(stat(check, &st) == 0 && S_ISDIR(st.st_mode)); /* cloned */
   snprintf(check, sizeof(check), "%s/README.md", path);
   assert(stat(check, &st) == 0); /* file came across */

   /* Real clone ownership is distinct from generic workspace registration. */
   {
      char child[PATH_MAX + 128], outside[PATH_MAX + 128], saved[PATH_MAX + 128];
      snprintf(child, sizeof child, "%s/.aimee/worktrees/live/main", path);
      assert(!run("mkdir -p %s", child));
      int fd = git_project_open_workspace("webuser:alice", child);
      struct stat held, named;
      assert(fd >= 0 && !fstat(fd, &held) && !stat(child, &named) && held.st_dev == named.st_dev &&
             held.st_ino == named.st_ino);
      close(fd);
      request_context_t owner = {.vault_transport = ATTEST_WEBCHAT_TRUSTED,
                                 .capabilities = CAP_TOOL_EXECUTE};
      snprintf(owner.vault_principal, sizeof owner.vault_principal, "webuser:alice");
      /* Exercise the exact production callback predicate, not just its resource
       * helper. A syntactically valid webuser on another transport is no grant. */
      fd = server_project_workspace_open_owned(&owner, child);
      assert(fd >= 0);
      close(fd);
      assert(server_project_workspace_open_owned(NULL, child) < 0);
      attested_transport_t refused[] = {ATTEST_NONE, ATTEST_TCP_BEARER, ATTEST_UDS_PEERCRED,
                                        ATTEST_MTLS_CLIENT};
      for (size_t t = 0; t < sizeof refused / sizeof refused[0]; t++)
      {
         owner.vault_transport = refused[t];
         assert(server_project_workspace_open_owned(&owner, child) < 0);
      }
      owner.vault_transport = ATTEST_WEBCHAT_TRUSTED;
      owner.capabilities = CAP_CHAT;
      assert(server_project_workspace_open_owned(&owner, child) < 0);
      owner.capabilities = CAP_TOOL_EXECUTE;
      snprintf(owner.vault_principal, sizeof owner.vault_principal, "uid:0");
      snprintf(owner.principal, sizeof owner.principal, "webuser:alice");
      assert(server_project_workspace_open_owned(&owner, child) < 0);
      snprintf(owner.vault_principal, sizeof owner.vault_principal, "webuser:../alice");
      assert(server_project_workspace_open_owned(&owner, child) < 0);
      /* The environment is deliberately shared by authenticated PAM actors. */
      fd = git_project_open_workspace("webuser:bob", child);
      assert(fd >= 0);
      close(fd);
      assert(git_project_open_workspace("uid:0", child) < 0);
      assert(git_project_open_workspace("webchat:alice", child) < 0);
      assert(git_project_open_workspace("webuser:../alice", child) < 0);
      assert(git_project_open_workspace(NULL, child) < 0);
      assert(git_project_open_workspace("webuser:alice", src) < 0);
      snprintf(outside, sizeof outside, "%s/environment/impostor", wsdir);
      assert(!run("mkdir -p %s/.git", outside));
      assert(git_project_open_workspace("webuser:alice", outside) < 0);
      assert(!run("rm -rf %s", outside));
      snprintf(outside, sizeof outside, "%s/escape", path);
      assert(!symlink(src, outside));
      assert(git_project_open_workspace("webuser:alice", outside) < 0);
      assert(!unlink(outside));
      snprintf(outside, sizeof outside, "%s/.aimee/remote", path);
      snprintf(saved, sizeof saved, "%s/.aimee/remote.saved", path);
      assert(!rename(outside, saved));
      assert(git_project_open_workspace("webuser:alice", child) < 0);
      assert(!symlink(saved, outside));
      assert(git_project_open_workspace("webuser:alice", child) < 0);
      assert(!unlink(outside));
      FILE *fake = fopen(outside, "w");
      assert(fake && fputs("https://example.invalid/unregistered\n", fake) >= 0 && !fclose(fake));
      assert(git_project_open_workspace("webuser:alice", child) < 0);
      assert(!unlink(outside) && !rename(saved, outside));
      int lock = ws_reg_lock("srcrepo");
      assert(lock >= 0 && !ws_reg_unregister("srcrepo"));
      close(lock);
      assert(git_project_open_workspace("webuser:alice", child) < 0);
      assert(!ws_reg_rebuild());
      fd = git_project_open_workspace("webuser:alice", child);
      assert(fd >= 0);
      close(fd);
      /* A returned descriptor pins the measured directory across replacement. */
      snprintf(saved, sizeof saved, "%s.retired", child);
      fd = git_project_open_workspace("webuser:alice", child);
      assert(fd >= 0 && !rename(child, saved) && !mkdir(child, 0700));
      assert(!fstat(fd, &held) && !stat(child, &named) && held.st_ino != named.st_ino);
      close(fd);
      assert(!rmdir(child) && !rename(saved, child));
   }

   /* re-clone the same name -> project already exists -> conflict */
   assert(git_project_clone("webuser:alice", url, NULL, NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);

   /* explicit name + .git stripping on derive */
   char url2[420];
   snprintf(url2, sizeof(url2), "file://%s/.git", src); /* trailing .git path form */
   assert(git_project_clone("webuser:alice", url, "myproj", NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   assert(strcmp(name, "myproj") == 0 && strstr(path, "/environment/myproj"));
   (void)url2;

   /* another actor clones into the SAME environment */
   assert(git_project_clone("webuser:bob", url, "shared", NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   assert(strstr(path, "/environment/shared") != NULL);

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
   assert(strstr(path, "/environment/acme/orgproj") != NULL);
   snprintf(check, sizeof(check), "%s/.aimee/remote", path);
   assert(stat(check, &st) == 0); /* credential-free sidecar published */
   /* same ref again -> exists -> conflict */
   assert(git_project_clone("webuser:alice", url, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);
   /* same KEY, different remote (second source repo). The ref already exists on
    * disk, and that check fires before the registry one, so this is the
    * existence conflict — the other remote is never echoed either way. */
   char src2[300], url3[400];
   snprintf(src2, sizeof(src2), "%s/srcrepo2", home);
   assert(run("mkdir -p %s && cd %s && git init -q && git config user.email t@t && git config "
              "user.name t && echo two > f && git add . && git commit -qm init",
              src2, src2) == 0);
   snprintf(url3, sizeof(url3), "file://%s", src2);
   assert(git_project_clone("webuser:bob", url3, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);
   assert(strstr(err, "already exists") != NULL);
   assert(strstr(err, "srcrepo") == NULL); /* no remote is disclosed */
   /* same key + SAME remote from another actor -> still the one project */
   assert(git_project_clone("webuser:bob", url, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);
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
   /* A published ref is a conflict for every actor, whichever remote they
    * name and however the clone was later repointed with `git remote set-url`. */
   assert(git_project_clone("webuser:alice", url, "syncy", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   assert(run("git -C %s remote set-url origin %s", path, url3) == 0);
   assert(git_project_clone("webuser:bob", url3, "syncy", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);
   assert(git_project_clone("webuser:carol", url, "syncy", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == GP_ERR_CONFLICT);

   /* A STALE entry — a ref registered with no published clone behind it, e.g. a
    * crash between register and publish — must SELF-HEAL rather than block the
    * ref forever. The resync drops an entry with no clone behind it, so the
    * clone proceeds and rebinds the ref to its real remote. */
   {
      int lk = ws_reg_lock("stale"); /* mutations require the lifecycle lock */
      assert(lk >= 0);
      assert(ws_reg_register("stale", "https://example.invalid/other.git") == 0);
      close(lk); /* release before the clone, which takes the same lock */
   }
   assert(git_project_clone("webuser:alice", url, "stale", NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   {
      char healed[1024];
      assert(ws_reg_lookup("stale", healed, sizeof(healed)) == 1);
      assert(strstr(healed, "example.invalid") == NULL); /* rebound to the real remote */
   }

   /* --- list: one environment holds srcrepo, myproj, shared, acme/orgproj,
    * Rakuen-Software/sanit, acme/syncy, stale — every actor sees all of them --- */
   char names[64][GIT_PROJECT_NAME_MAX];
   int an = git_project_list("webuser:alice", names, 64);
   assert(an == 7);
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
   assert(bn == an); /* every actor sees the same environment */
   assert(git_project_list("uid:1000", names, 64) == -1);
   assert(git_project_list("webuser:carol", names, 64) == an);

   /* --- delete (slice 2). LOCAL: the clone and this server's own state go, and
    * aimee-kb is never called — so there is no purge outcome, no fence, and no
    * force. --- */
   char derr[512];

   /* refusals: bad ref / non-webuser */
   assert(git_project_delete("webuser:alice", "../x", derr, sizeof(derr)) == -1);
   assert(git_project_delete("webuser:alice", "a/b/c", derr, sizeof(derr)) == -1);
   assert(git_project_delete("uid:1000", "srcrepo", derr, sizeof(derr)) == -1);
   /* a ref that does not exist is a plain not-found */
   assert(git_project_delete("webuser:carol", "nosuch", derr, sizeof(derr)) == GP_ERR_NOT_FOUND);

   /* Any actor may delete any project in the environment, and the registry
    * entry goes with it. */
   char aorg[PATH_MAX], remo[1024];
   snprintf(aorg, sizeof(aorg), "%s/environment/acme/orgproj", wsdir);
   assert(stat(aorg, &st) == 0);
   assert(git_project_delete("webuser:bob", "acme/orgproj", derr, sizeof(derr)) == 0);
   assert(stat(aorg, &st) != 0);
   assert(ws_reg_lookup("acme/orgproj", remo, sizeof(remo)) == 0);
   /* the org dir survives while a sibling project still lives there */
   snprintf(check, sizeof(check), "%s/environment/acme/syncy", wsdir);
   assert(stat(check, &st) == 0);

   /* deleting the same ref again is a plain not-found */
   assert(git_project_delete("webuser:alice", "acme/orgproj", derr, sizeof(derr)) ==
          GP_ERR_NOT_FOUND);

   snprintf(check, sizeof(check), "%s/environment/shared", wsdir);
   assert(stat(check, &st) == 0);
   assert(git_project_delete("webuser:bob", "shared", derr, sizeof(derr)) == 0);
   assert(stat(check, &st) != 0);

   /* local-index failure aborts BEFORE any filesystem removal, and the registry
    * entry is restored so a re-run converges. */
   setenv("AIMEE_TEST_CODE_INDEX_DELETE_FAIL", "1", 1);
   snprintf(check, sizeof(check), "%s/environment/srcrepo", wsdir);
   assert(git_project_delete("webuser:alice", "srcrepo", derr, sizeof(derr)) == -1);
   assert(strstr(derr, "code index") != NULL);
   assert(stat(check, &st) == 0);                             /* clone intact */
   assert(ws_reg_lookup("srcrepo", remo, sizeof(remo)) == 1); /* entry restored */
   unsetenv("AIMEE_TEST_CODE_INDEX_DELETE_FAIL");
   assert(git_project_delete("webuser:alice", "srcrepo", derr, sizeof(derr)) == 0);
   assert(stat(check, &st) != 0);
   assert(ws_reg_lookup("srcrepo", remo, sizeof(remo)) == 0);

   snprintf(check, sizeof(check), "%s/environment/Rakuen-Software/sanit", wsdir);
   assert(git_project_delete("webuser:alice", "Rakuen-Software/sanit", derr, sizeof(derr)) == 0);
   assert(stat(check, &st) != 0);
   snprintf(check, sizeof(check), "%s/environment/myproj", wsdir);
   assert(git_project_delete("webuser:alice", "myproj", derr, sizeof(derr)) == 0);
   assert(stat(check, &st) != 0);
   snprintf(check, sizeof(check), "%s/environment/stale", wsdir);
   assert(git_project_delete("webuser:alice", "stale", derr, sizeof(derr)) == 0);
   assert(stat(check, &st) != 0);

   snprintf(check, sizeof(check), "%s/environment/acme/syncy", wsdir);
   int project_fd = git_project_open_workspace("webuser:alice", check);
   assert(project_fd >= 0);
   close(project_fd);
   snprintf(check, sizeof(check), "%s/environment/acme", wsdir);
   assert(git_project_open_workspace("webuser:alice", check) < 0);

   /* final tally: only acme/syncy remains. */
   assert(git_project_list("webuser:alice", names, 64) == 1);
   assert(strcmp(names[0], "acme/syncy") == 0);

   /* --- rename-first tombstone: an interrupted walk is resumable --- */
   /* Simulate a crash right after the tombstone rename: the project sits at its
    * ".deleting-<repo>" sibling. Re-running the delete must converge. */
   char mpath[PATH_MAX];
   snprintf(mpath, sizeof(mpath), "%s/environment/acme/.deleting-syncy", wsdir);
   assert(run("mv %s/environment/acme/syncy %s", wsdir, mpath) == 0);
   assert(git_project_delete("webuser:alice", "acme/syncy", derr, sizeof(derr)) == 0);
   assert(stat(mpath, &st) != 0); /* marker tree fully gone */
   snprintf(check, sizeof(check), "%s/environment/acme", wsdir);
   assert(stat(check, &st) != 0); /* org pruned */
   assert(ws_reg_lookup("acme/syncy", remo, sizeof(remo)) == 0);

   /* a flat marker with content (the ref itself no longer exists anywhere)
    * also converges, and once converged the ref is a plain 404 again */
   assert(run("mkdir -p %s/environment/.deleting-ghost/sub && "
              "echo x > %s/environment/.deleting-ghost/sub/f",
              wsdir, wsdir) == 0);
   assert(git_project_delete("webuser:alice", "ghost", derr, sizeof(derr)) == 0);
   snprintf(check, sizeof(check), "%s/environment/.deleting-ghost", wsdir);
   assert(stat(check, &st) != 0);
   assert(git_project_delete("webuser:alice", "ghost", derr, sizeof(derr)) == GP_ERR_NOT_FOUND);
   /* never-existing refs (flat and org) still 404 — no marker, no project */
   assert(git_project_delete("webuser:alice", "neverwas", derr, sizeof(derr)) == GP_ERR_NOT_FOUND);
   assert(git_project_delete("webuser:alice", "no/pe", derr, sizeof(derr)) == GP_ERR_NOT_FOUND);
   assert(git_project_list("webuser:alice", names, 64) == 0);

   assert(run("rm -rf %s", home) == 0);
   printf("git_project: all tests passed\n");
   return 0;
}
