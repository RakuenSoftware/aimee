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
   memset(n64, 'a', 64);
   n64[64] = '\0';
   memset(n65, 'a', 65);
   n65[65] = '\0';
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
   /* bad project names rejected ("a/b" is now a VALID org/repo ref — the
    * two-component cases are exercised in the ref sections below) */
   assert(ws_scope_project_path("webuser:alice", "..", 0, proj, sizeof(proj)) == -1);
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

   /* --- ws_scope_project_ref_valid: the only '/'-accepting validator --- */
   {
      /* accepted: flat and exactly org/repo */
      assert(ws_scope_project_ref_valid("foo", 3));
      assert(ws_scope_project_ref_valid("acme/foo", 8));
      assert(ws_scope_project_ref_valid("a-b.c_d/e.f", 11));
      /* rejected: traversal, nesting, hidden, absolute, empty segments */
      assert(!ws_scope_project_ref_valid("../x", 4));
      assert(!ws_scope_project_ref_valid("a/../b", 6));
      assert(!ws_scope_project_ref_valid("a//b", 4));
      assert(!ws_scope_project_ref_valid(".hidden/x", 9));
      assert(!ws_scope_project_ref_valid("a/.hidden", 9));
      assert(!ws_scope_project_ref_valid("/abs", 4));
      assert(!ws_scope_project_ref_valid("a/b/c", 5));
      assert(!ws_scope_project_ref_valid("a/", 2));
      assert(!ws_scope_project_ref_valid("/a", 2));
      assert(!ws_scope_project_ref_valid("", 0));
      assert(!ws_scope_project_ref_valid(NULL, 0));
      /* embedded NUL by byte-scan (buffer not NUL-terminated at len) */
      assert(!ws_scope_project_ref_valid("a\0b", 3));
      assert(!ws_scope_project_ref_valid("acme/f\0o", 8));
      /* control bytes + non-ASCII rejected via ws_scope_name_valid, both
       * component positions */
      assert(!ws_scope_project_ref_valid("a\x01"
                                         "b/c",
                                         5));
      assert(!ws_scope_project_ref_valid("a/b\x7f"
                                         "c",
                                         5));
      assert(!ws_scope_project_ref_valid("caf\xc3\xa9/x", 7));
      /* length caps: >129 total, >64 per component */
      char big[200];
      memset(big, 'a', sizeof(big));
      assert(!ws_scope_project_ref_valid(big, 130));
      char comp65[70];
      memset(comp65, 'a', 65);
      comp65[65] = '/';
      comp65[66] = 'x';
      assert(!ws_scope_project_ref_valid(comp65, 67));
      /* 64/64 with '/' = 129 total is the maximum accepted shape */
      char maxref[130];
      memset(maxref, 'a', 64);
      maxref[64] = '/';
      memset(maxref + 65, 'b', 64);
      assert(ws_scope_project_ref_valid(maxref, 129));
   }

   /* --- ws_scope_ref_split --- */
   {
      char org[65], repo[65];
      assert(ws_scope_ref_split("foo", org, sizeof(org), repo, sizeof(repo)) == 1);
      assert(org[0] == '\0' && strcmp(repo, "foo") == 0);
      assert(ws_scope_ref_split("acme/foo", org, sizeof(org), repo, sizeof(repo)) == 2);
      assert(strcmp(org, "acme") == 0 && strcmp(repo, "foo") == 0);
      assert(ws_scope_ref_split("a/../b", org, sizeof(org), repo, sizeof(repo)) == -1);
   }

   /* --- two-component refs through project_path / open_project --- */
   {
      char orgdir[PATH_MAX], nested[PATH_MAX];
      snprintf(orgdir, sizeof(orgdir), "%s/acme", rootA);
      assert(mkdir(orgdir, 0700) == 0);
      snprintf(nested, sizeof(nested), "%s/acme/app", rootA);
      assert(mkdir(nested, 0700) == 0);

      char got[PATH_MAX];
      assert(ws_scope_project_path("webuser:alice", "acme/app", 1, got, sizeof(got)) == 0);
      assert(strstr(got, "/acme/app") != NULL);
      /* clone-target mode: nested ref that exists is refused */
      assert(ws_scope_project_path("webuser:alice", "acme/app", 0, got, sizeof(got)) == -1);
      /* traversal shapes rejected before any resolution */
      assert(ws_scope_project_path("webuser:alice", "acme/../app", 1, got, sizeof(got)) == -1);

      int nfd = ws_scope_open_project("webuser:alice", "acme/app", 0);
      assert(nfd >= 0);
      close(nfd);
      assert(ws_scope_open_project("webuser:alice", "acme/../app", 0) == -1);
      /* symlink planted as the org component is rejected (O_NOFOLLOW) */
      char orglink[PATH_MAX];
      snprintf(orglink, sizeof(orglink), "%s/evilorg", rootA);
      assert(symlink("/", orglink) == 0);
      assert(ws_scope_open_project("webuser:alice", "evilorg/etc", 0) == -1);
      unlink(orglink);

      rmdir(nested);
      rmdir(orgdir);
   }

   /* cleanup */
   rmdir(real_proj);
   unlink(evil);
   rmdir(rootA);
   rmdir(rootB);

   printf("workspace_scope: all tests passed\n");
   return 0;
}
