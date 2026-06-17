/* test_workspace_scope.c — per-webuser workspace isolation (WP-A).
 * Drives ws_scope_* against a real tmp AIMEE_WORKSPACES_DIR: name validation,
 * per-user root creation (0700), project resolution within the root, and the
 * security properties — cross-principal separation, '..'/'/' rejection, and
 * symlink-escape rejection. */
#include "workspace_scope.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
   char dir[] = "/tmp/ws_scope.XXXXXX";
   assert(mkdtemp(dir) != NULL);
   setenv("AIMEE_WORKSPACES_DIR", dir, 1);

   /* --- name validation --- */
   assert(ws_scope_name_valid("alice"));
   assert(ws_scope_name_valid("a.b_c-1"));
   assert(!ws_scope_name_valid(""));
   assert(!ws_scope_name_valid("."));
   assert(!ws_scope_name_valid(".."));
   assert(!ws_scope_name_valid(".hidden")); /* leading dot */
   assert(!ws_scope_name_valid("-flag"));   /* leading dash */
   assert(!ws_scope_name_valid("a/b"));     /* slash */
   assert(!ws_scope_name_valid("a b"));     /* space */
   assert(!ws_scope_name_valid("a\tb"));
   /* length boundary: 64 ok, 65 rejected */
   char n64[65], n65[66];
   memset(n64, 'a', 64); n64[64] = '\0';
   memset(n65, 'a', 65); n65[65] = '\0';
   assert(ws_scope_name_valid(n64));
   assert(!ws_scope_name_valid(n65));

   /* cap==0 / NULL out are rejected, not written */
   char tiny[1];
   assert(ws_scope_user_root("webuser:alice", 0, tiny, 0) == -1);
   assert(ws_scope_user_root("webuser:alice", 0, NULL, 16) == -1);

   /* --- user root: only webuser: principals, created 0700 --- */
   char rootA[PATH_MAX], rootB[PATH_MAX];
   assert(ws_scope_user_root("uid:1000", 1, rootA, sizeof(rootA)) == -1); /* not a webuser */
   assert(ws_scope_user_root("webuser:..", 1, rootA, sizeof(rootA)) == -1);
   assert(ws_scope_user_root("webuser:a/b", 1, rootA, sizeof(rootA)) == -1);

   assert(ws_scope_user_root("webuser:alice", 1, rootA, sizeof(rootA)) == 0);
   assert(ws_scope_user_root("webuser:bob", 1, rootB, sizeof(rootB)) == 0);
   assert(strcmp(rootA, rootB) != 0);
   struct stat st;
   assert(stat(rootA, &st) == 0 && S_ISDIR(st.st_mode));
   assert((st.st_mode & 0777) == 0700); /* private */
   assert(strstr(rootA, "/webusers/alice") != NULL);

   /* --- project path: not-yet-existing clone target --- */
   char proj[PATH_MAX];
   assert(ws_scope_project_path("webuser:alice", "myrepo", 0, proj, sizeof(proj)) == 0);
   assert(strncmp(proj, rootA, strlen(rootA)) == 0 && strstr(proj, "/myrepo"));
   /* bad project names rejected */
   assert(ws_scope_project_path("webuser:alice", "..", 0, proj, sizeof(proj)) == -1);
   assert(ws_scope_project_path("webuser:alice", "a/b", 0, proj, sizeof(proj)) == -1);
   assert(ws_scope_project_path("webuser:alice", "../bob", 0, proj, sizeof(proj)) == -1);

   /* materialize the project, then must_exist resolution must succeed + stay in root */
   char real_proj[PATH_MAX];
   snprintf(real_proj, sizeof(real_proj), "%s/myrepo", rootA);
   assert(mkdir(real_proj, 0700) == 0);
   assert(ws_scope_project_path("webuser:alice", "myrepo", 1, proj, sizeof(proj)) == 0);
   assert(ws_scope_contains("webuser:alice", proj) == 1);
   /* alice's project is NOT within bob's scope */
   assert(ws_scope_contains("webuser:bob", proj) == 0);

   /* --- symlink escape rejection --- */
   /* alice/evil -> bob's root. must_exist resolution must reject it (realpath
    * lands outside alice's canonical root). */
   char evil[PATH_MAX], target[PATH_MAX];
   snprintf(evil, sizeof(evil), "%s/evil", rootA);
   snprintf(target, sizeof(target), "%s", rootB);
   assert(symlink(target, evil) == 0);
   assert(ws_scope_project_path("webuser:alice", "evil", 1, proj, sizeof(proj)) == -1);
   /* a symlink also blocks a clone target of the same name (lstat detects it) */
   assert(ws_scope_project_path("webuser:alice", "evil", 0, proj, sizeof(proj)) == -1);
   /* and ws_scope_contains on the symlink's resolved (bob) path is false for alice */
   assert(ws_scope_contains("webuser:alice", rootB) == 0);

   /* --- prefix false-positive: a sibling dir sharing a name prefix with the
    * root must NOT count as "within" (/.../alice vs /.../alicex boundary) --- */
   {
      char sibling[PATH_MAX];
      /* rootA ends in "/alice"; craft "/aliceX" sibling */
      snprintf(sibling, sizeof(sibling), "%sX", rootA);
      assert(mkdir(sibling, 0700) == 0);
      assert(ws_scope_contains("webuser:alice", sibling) == 0); /* not within */
      rmdir(sibling);
   }

   /* --- safe openat base fd + TOCTOU-free project open --- */
   int fd = ws_scope_open_user_root("webuser:alice");
   assert(fd >= 0);
   close(fd);
   assert(ws_scope_open_user_root("uid:1000") == -1); /* invalid principal */
   /* open the real project via the openat API */
   int pfd = ws_scope_open_project("webuser:alice", "myrepo", 0);
   assert(pfd >= 0);
   close(pfd);
   /* O_NOFOLLOW rejects opening through the planted escape symlink */
   assert(ws_scope_open_project("webuser:alice", "evil", 0) == -1);
   assert(ws_scope_open_project("webuser:alice", "..", 0) == -1);

   /* cleanup */
   rmdir(real_proj);
   unlink(evil);
   rmdir(rootA);
   rmdir(rootB);

   printf("workspace_scope: all tests passed\n");
   return 0;
}
