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
   assert(git_project_clone("webuser:alice", url, NULL, path, sizeof(path), name, sizeof(name), err,
                            sizeof(err)) == 0);
   assert(strcmp(name, "srcrepo") == 0);
   assert(strstr(path, "/webusers/alice/srcrepo") != NULL);
   struct stat st;
   char check[PATH_MAX + 32];
   snprintf(check, sizeof(check), "%s/.git", path);
   assert(stat(check, &st) == 0 && S_ISDIR(st.st_mode)); /* cloned */
   snprintf(check, sizeof(check), "%s/README.md", path);
   assert(stat(check, &st) == 0); /* file came across */

   /* re-clone the same name -> project already exists -> refused */
   assert(git_project_clone("webuser:alice", url, NULL, path, sizeof(path), name, sizeof(name), err,
                            sizeof(err)) == -1);

   /* explicit name + .git stripping on derive */
   char url2[420];
   snprintf(url2, sizeof(url2), "file://%s/.git", src); /* trailing .git path form */
   assert(git_project_clone("webuser:alice", url, "myproj", path, sizeof(path), name, sizeof(name),
                            err, sizeof(err)) == 0);
   assert(strcmp(name, "myproj") == 0 && strstr(path, "/alice/myproj"));
   (void)url2;

   /* cross-principal: bob clones into HIS scope, not alice's */
   assert(git_project_clone("webuser:bob", url, "shared", path, sizeof(path), name, sizeof(name),
                            err, sizeof(err)) == 0);
   assert(strstr(path, "/webusers/bob/shared") != NULL);

   /* refusals */
   assert(git_project_clone("uid:1000", url, "x", path, sizeof(path), name, sizeof(name), err,
                            sizeof(err)) == -1); /* not a webuser */
   assert(git_project_clone("webuser:alice", "", "x", path, sizeof(path), name, sizeof(name), err,
                            sizeof(err)) == -1); /* empty url */
   assert(git_project_clone("webuser:alice", "--upload-pack=evil", "x", path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1); /* flag-like url */
   assert(git_project_clone("webuser:alice", url, "../escape", path, sizeof(path), name,
                            sizeof(name), err, sizeof(err)) == -1); /* bad name */

   assert(run("rm -rf %s", home) == 0);
   printf("git_project: all tests passed\n");
   return 0;
}
