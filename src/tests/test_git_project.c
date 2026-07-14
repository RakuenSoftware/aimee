/* test_git_project.c — WP-D: clone a repo as a project under a webuser's scoped
 * workspace. Uses a local file:// source repo (no network, no creds) created in
 * the test, so it exercises the real git clone + scope resolution + name
 * derivation + refusal paths deterministically. */
#include "git_project.h"

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

   /* re-clone the same name -> project already exists -> refused */
   assert(git_project_clone("webuser:alice", url, NULL, NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1);

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
   /* same ref again -> exists -> refused */
   assert(git_project_clone("webuser:alice", url, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1);
   /* same KEY, different remote (second source repo) -> registry 409, generic
    * message (no other remote echoed) */
   char src2[300], url3[400];
   snprintf(src2, sizeof(src2), "%s/srcrepo2", home);
   assert(run("mkdir -p %s && cd %s && git init -q && git config user.email t@t && git config "
              "user.name t && echo two > f && git add . && git commit -qm init",
              src2, src2) == 0);
   snprintf(url3, sizeof(url3), "file://%s", src2);
   assert(git_project_clone("webuser:bob", url3, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1);
   assert(strstr(err, "different remote") != NULL);
   assert(strstr(err, "srcrepo") == NULL); /* the other remote is NOT disclosed */
   /* same key + SAME remote from another webuser -> allowed (holder count 2) */
   assert(git_project_clone("webuser:bob", url, "orgproj", "acme", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == 0);
   assert(strstr(path, "/webusers/bob/acme/orgproj") != NULL);
   /* flat/org namespace conflict: a flat project named like the org */
   assert(git_project_clone("webuser:alice", url3, "acme", NULL, NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1); /* org dir 'acme' exists */
   /* org named like an existing flat project */
   assert(git_project_clone("webuser:alice", url3, "x2", "srcrepo", NULL, path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1);
   /* org override sanitization: "Rakuen Software!" -> "Rakuen-Software" */
   assert(git_project_clone("webuser:alice", url3, "sanit", "Rakuen Software!", NULL, path,
                            sizeof(path), name, sizeof(name), err, sizeof(err)) == 0);
   assert(strcmp(name, "Rakuen-Software/sanit") == 0);

   /* --- list: alice has srcrepo + myproj + acme/orgproj + Rakuen-Software/sanit;
    * bob has shared + acme/orgproj; isolated per user --- */
   char names[64][GIT_PROJECT_NAME_MAX];
   int an = git_project_list("webuser:alice", names, 64);
   assert(an == 4);
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
   assert(bn == 2); /* bob sees only his: shared + acme/orgproj */
   assert(git_project_list("uid:1000", names, 64) == -1);
   /* a webuser with no clones lists zero, not an error */
   assert(git_project_list("webuser:carol", names, 64) == 0);

   assert(run("rm -rf %s", home) == 0);
   printf("git_project: all tests passed\n");
   return 0;
}
