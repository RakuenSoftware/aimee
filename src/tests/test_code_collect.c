/* test_code_collect.c: integration tests for the code collector's source
 * selection. Code indexing must read the git DEFAULT branch (canonical code),
 * not the user's working tree / feature-branch WIP. Each test materializes a
 * throwaway git repo under TMPDIR via the real `git` binary (already a build
 * prerequisite) and drives code_collect_files_cb() against it. */
#include "code_collect.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- collected-file recorder ---- */
#define MAX_FILES 64
static char g_paths[MAX_FILES][512];
static char g_content[MAX_FILES][512];
static int g_n;

static void reset(void)
{
   g_n = 0;
}
static int rec_cb(const char *rel, const char *content, void *ctx)
{
   (void)ctx;
   if (g_n < MAX_FILES)
   {
      snprintf(g_paths[g_n], sizeof(g_paths[g_n]), "%s", rel);
      snprintf(g_content[g_n], sizeof(g_content[g_n]), "%s", content);
      g_n++;
   }
   return 0;
}
static const char *content_of(const char *rel)
{
   for (int i = 0; i < g_n; i++)
      if (strcmp(g_paths[i], rel) == 0)
         return g_content[i];
   return NULL;
}
static int has(const char *rel)
{
   return content_of(rel) != NULL;
}

/* ---- temp-repo scaffolding ---- */
static char g_root[1024];

static void sh(const char *fmt, ...)
{
   char cmd[4096];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(cmd, sizeof(cmd), fmt, ap);
   va_end(ap);
   int rc = system(cmd);
   assert(rc == 0);
}

static void write_file(const char *rel, const char *content)
{
   char path[2048];
   snprintf(path, sizeof(path), "%s/%s", g_root, rel);
   /* ensure parent dir */
   char dir[2048];
   snprintf(dir, sizeof(dir), "%s", path);
   char *slash = strrchr(dir, '/');
   if (slash)
   {
      *slash = '\0';
      sh("mkdir -p '%s'", dir);
   }
   FILE *fp = fopen(path, "wb");
   assert(fp);
   fputs(content, fp);
   fclose(fp);
}

/* Read a whole file into a malloc'd NUL-terminated buffer (caller frees), or NULL. */
static char *read_file(const char *path)
{
   FILE *fp = fopen(path, "rb");
   if (!fp)
      return NULL;
   fseek(fp, 0, SEEK_END);
   long n = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (n < 0)
   {
      fclose(fp);
      return NULL;
   }
   char *buf = malloc((size_t)n + 1);
   if (buf)
   {
      size_t got = fread(buf, 1, (size_t)n, fp);
      buf[got] = '\0';
   }
   fclose(fp);
   return buf;
}

static void make_root(const char *name)
{
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !tmp[0])
      tmp = "/tmp";
   snprintf(g_root, sizeof(g_root), "%s/aimee-cct-%s-%d", tmp, name, (int)getpid());
   sh("rm -rf '%s' && mkdir -p '%s'", g_root, g_root);
}

static void git(const char *args)
{
   sh("git -C '%s' %s >/dev/null 2>&1", g_root, args);
}

/* The default branch is canonical: WIP on a feature branch + uncommitted edits
 * must never leak into the indexed view. */
static void test_default_branch_is_canonical(void)
{
   make_root("canon");
   git("init -q -b main");
   git("config user.email t@t");
   git("config user.name t");
   write_file("src/main.c", "int canonical(void){return 0;}");
   write_file("README.md", "# canonical readme");
   write_file("src/vendor/lib.c", "vendored, tracked, must be skipped");
   git("add -A");
   git("commit -qm init");
   /* diverge: feature branch + uncommitted working-tree garbage */
   git("checkout -q -b feature");
   write_file("src/main.c", "GARBAGE WIP MUST NOT INDEX");
   write_file("src/untracked.c", "uncommitted untracked");

   reset();
   int n = code_collect_files_cb(g_root, rec_cb, NULL);

   assert(n == 2); /* src/main.c + README.md; vendor/ skipped, untracked absent */
   assert(has("src/main.c") && has("README.md"));
   assert(strcmp(content_of("src/main.c"), "int canonical(void){return 0;}") == 0);
   assert(!has("src/vendor/lib.c")); /* vendor path component filtered */
   assert(!has("src/untracked.c"));  /* working-tree-only file not on the branch */
   printf("  test_default_branch_is_canonical: ok\n");
}

/* AIMEE_CODE_INDEX_SOURCE=worktree is the documented opt-in to index WIP. */
static void test_worktree_optin(void)
{
   make_root("wt");
   git("init -q -b main");
   git("config user.email t@t");
   git("config user.name t");
   write_file("src/main.c", "int canonical(void){return 0;}");
   git("add -A");
   git("commit -qm init");
   git("checkout -q -b feature");
   write_file("src/main.c", "WIP EDIT");
   write_file("src/untracked.c", "untracked wip");

   setenv("AIMEE_CODE_INDEX_SOURCE", "worktree", 1);
   reset();
   int n = code_collect_files_cb(g_root, rec_cb, NULL);
   unsetenv("AIMEE_CODE_INDEX_SOURCE");

   assert(n == 2); /* working tree: the edited main.c + the untracked file */
   assert(strcmp(content_of("src/main.c"), "WIP EDIT") == 0);
   assert(has("src/untracked.c"));
   printf("  test_worktree_optin: ok\n");
}

/* origin/HEAD is resolved (and repaired if unset) so a clone indexes the remote
 * default even while checked out on a local feature branch. */
static void test_clone_resolves_origin_head(void)
{
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !tmp[0])
      tmp = "/tmp";
   char up[1024];
   snprintf(up, sizeof(up), "%s/aimee-cct-up-%d", tmp, (int)getpid());
   sh("rm -rf '%s'", up);
   sh("git init -q -b master '%s'", up);
   sh("git -C '%s' config user.email t@t && git -C '%s' config user.name t", up, up);
   sh("printf 'canonical on master' > '%s/app.py'", up);
   sh("git -C '%s' add -A && git -C '%s' commit -qm init >/dev/null 2>&1", up, up);

   make_root("clone");
   sh("git clone -q '%s' '%s'", up, g_root);
   git("checkout -q -b mybranch");
   write_file("app.py", "WIP");
   git("symbolic-ref -d refs/remotes/origin/HEAD"); /* force the repair path */

   reset();
   int n = code_collect_files_cb(g_root, rec_cb, NULL);
   sh("rm -rf '%s'", up);

   assert(n == 1);
   assert(strcmp(content_of("app.py"), "canonical on master") == 0); /* not "WIP" */
   printf("  test_clone_resolves_origin_head: ok\n");
}

/* A git repo with no resolvable default branch is SKIPPED, never silently
 * indexed from the working tree. */
static void test_no_default_branch_skips(void)
{
   make_root("nobranch");
   git("init -q -b main");
   write_file("x.c", "int x;"); /* never committed: no main/master ref exists */

   reset();
   int n = code_collect_files_cb(g_root, rec_cb, NULL);
   assert(n == 0 && g_n == 0);
   printf("  test_no_default_branch_skips: ok\n");
}

/* A non-git directory falls back to the working-tree walk unconditionally. */
static void test_non_git_uses_worktree(void)
{
   make_root("nongit");
   write_file("a.py", "plain = 1");

   reset();
   int n = code_collect_files_cb(g_root, rec_cb, NULL);
   assert(n == 1 && has("a.py"));
   printf("  test_non_git_uses_worktree: ok\n");
}

/* R2: build manifests (CMakeLists.txt, *.cmake, .gitmodules, meson.build) are
 * collected so the build-declared-edge pass can read their content; a non-manifest
 * .txt is NOT collected. */
static void test_build_manifests_collected(void)
{
   make_root("buildman");
   write_file("CMakeLists.txt", "FetchContent_Declare(dep GIT_REPOSITORY x)");
   write_file("cmake/deps.cmake", "find_package(foo)");
   write_file(".gitmodules", "[submodule \"x\"]");
   write_file("meson.build", "project('p')");
   write_file("src/a.cpp", "int a(){return 0;}");
   write_file("README.txt", "not a manifest");
   /* a CMakeLists.txt under a build-output dir must NOT be collected (the walk skips
    * build/ at directory recursion, so FetchContent'd _deps manifests never enter). */
   write_file("build/_deps/dep-src/CMakeLists.txt", "FetchContent_Declare(other GIT_REPOSITORY y)");

   reset();
   code_collect_files_cb(g_root, rec_cb, NULL);
   assert(has("CMakeLists.txt"));
   assert(has("cmake/deps.cmake"));
   assert(has(".gitmodules"));
   assert(has("meson.build"));
   assert(has("src/a.cpp"));
   assert(!has("README.txt"));                         /* plain .txt is not a build manifest */
   assert(!has("build/_deps/dep-src/CMakeLists.txt")); /* build-output subtree skipped */
   printf("  test_build_manifests_collected: ok\n");
}

/* R2: build manifests are collected via the GIT-TRACKED path too (the second
 * collection site), and a build/ manifest stays excluded there as well. */
static void test_build_manifests_collected_git(void)
{
   make_root("buildman_git");
   git("init -q -b main");
   git("config user.email t@t");
   git("config user.name t");
   write_file("CMakeLists.txt", "FetchContent_Declare(dep GIT_REPOSITORY x)");
   write_file("src/a.cpp", "int a(){return 0;}");
   write_file("build/CMakeLists.txt", "generated");
   git("add -A -f"); /* -f: build/ may be gitignored in some setups; force-track for the test */
   git("commit -qm c");

   reset();
   code_collect_files_cb(g_root, rec_cb, NULL);
   assert(has("CMakeLists.txt"));
   assert(has("src/a.cpp"));
   assert(!has("build/CMakeLists.txt")); /* build-output dir excluded on the git path too */
   printf("  test_build_manifests_collected_git: ok\n");
}

/* §6 live: the default-branch tree SHA tracks commits, and the pure change-gate
 * decides when a re-index is warranted. */
static void test_default_branch_sha_tracks_commits(void)
{
   make_root("sha");
   git("init -q -b main");
   git("config user.email t@t");
   git("config user.name t");
   write_file("src/a.c", "int a(void){return 1;}");
   git("add -A");
   git("commit -qm c1");

   char sha1[128] = "", sha1b[128] = "";
   assert(git_resolve_default_sha(g_root, sha1, sizeof(sha1)) == 0 && sha1[0]);
   assert(git_resolve_default_sha(g_root, sha1b, sizeof(sha1b)) == 0);
   assert(strcmp(sha1, sha1b) == 0); /* stable when nothing moved */

   write_file("src/b.c", "int b(void){return 2;}");
   git("add -A");
   git("commit -qm c2");
   char sha2[128] = "";
   assert(git_resolve_default_sha(g_root, sha2, sizeof(sha2)) == 0);
   assert(strcmp(sha1, sha2) != 0); /* a commit moves the tree SHA */

   assert(code_default_branch_changed(sha1, sha2) == 1); /* moved -> reindex */
   assert(code_default_branch_changed(sha1, sha1) == 0); /* unchanged -> skip */
   assert(code_default_branch_changed("", sha1) == 1);   /* never indexed -> reindex */
   assert(code_default_branch_changed(sha1, "") == 0);   /* unresolved -> don't thrash */
   printf("  test_default_branch_sha_tracks_commits: ok\n");
}

/* Regression: a default branch whose NAME contains a single quote must resolve
 * correctly (and never break out of the shell) — the ref is shquoted, not hand-
 * wrapped. Set up via a clone so origin/HEAD points at the quote-named branch. */
static void test_default_branch_sha_quote_in_ref(void)
{
   make_root("qsha");
   char up[1200];
   snprintf(up, sizeof(up), "%s-rem", g_root);
   sh("rm -rf '%s' '%s'", up, g_root); /* clone needs a non-existent / empty target */
   sh("git init -q -b \"wip'x\" '%s'", up);
   sh("git -C '%s' config user.email t@t && git -C '%s' config user.name t", up, up);
   sh("printf 'int q(void){return 0;}' > '%s/q.c'", up);
   sh("git -C '%s' add -A && git -C '%s' commit -qm c", up, up);
   sh("git clone -q '%s' '%s'", up, g_root);

   char sha[128] = "";
   int rc = git_resolve_default_sha(g_root, sha, sizeof(sha));
   assert(rc == 0 && strlen(sha) >= 7); /* resolved despite the quote in the ref name */
   sh("rm -rf '%s'", up);
   printf("  test_default_branch_sha_quote_in_ref: ok\n");
}

/* The worktree-source predicate reflects AIMEE_CODE_INDEX_SOURCE (gates off the
 * default-branch SHA optimization when the index tracks WIP). */
static void test_index_source_is_worktree(void)
{
   unsetenv("AIMEE_CODE_INDEX_SOURCE");
   assert(code_index_source_is_worktree() == 0);
   setenv("AIMEE_CODE_INDEX_SOURCE", "default", 1);
   assert(code_index_source_is_worktree() == 0);
   setenv("AIMEE_CODE_INDEX_SOURCE", "worktree", 1);
   assert(code_index_source_is_worktree() == 1);
   unsetenv("AIMEE_CODE_INDEX_SOURCE");
   printf("  test_index_source_is_worktree: ok\n");
}

/* §6 live: the post-merge hook installs, is executable, carries the marker + the
 * backgrounded scan command for this project, is idempotent, and won't clobber a
 * foreign hook. */
static void test_install_branch_hook(void)
{
   make_root("hook");
   git("init -q -b main");
   char hook[2048];
   snprintf(hook, sizeof(hook), "%s/.git/hooks/post-merge", g_root);

   int rc = code_index_install_branch_hook(g_root, "proj-alpha");
   assert(rc == 0);
   struct stat st;
   assert(stat(hook, &st) == 0);
   assert(st.st_mode & S_IXUSR); /* executable */

   char *body = read_file(hook);
   assert(body);
   assert(strstr(body, "installed by aimee"));
   assert(strstr(body, "aimee index scan 'proj-alpha'"));
   assert(strstr(body, "&")); /* backgrounded */
   free(body);

   /* post-checkout is installed too, gated so only a branch switch ($3 == 1) reindexes. */
   char co[2048];
   snprintf(co, sizeof(co), "%s/.git/hooks/post-checkout", g_root);
   struct stat cst;
   assert(stat(co, &cst) == 0);
   assert(cst.st_mode & S_IXUSR);
   char *cbody = read_file(co);
   assert(cbody);
   assert(strstr(cbody, "installed by aimee"));
   assert(strstr(cbody, "aimee index scan 'proj-alpha'"));
   assert(strstr(cbody, "\"$3\" = \"1\"")); /* branch-switch gate */
   free(cbody);

   /* idempotent: re-installing our own hooks succeeds. */
   assert(code_index_install_branch_hook(g_root, "proj-alpha") == 0);

   /* a foreign hook is not clobbered (-2). */
   FILE *f = fopen(hook, "w");
   assert(f);
   fputs("#!/bin/sh\necho not aimee\n", f);
   fclose(f);
   assert(code_index_install_branch_hook(g_root, "proj-alpha") == -2);
   char *foreign = read_file(hook);
   assert(foreign && strstr(foreign, "not aimee") && !strstr(foreign, "installed by aimee"));
   free(foreign);

   /* a non-git dir fails cleanly. */
   make_root("hook-nogit");
   assert(code_index_install_branch_hook(g_root, "p") == -1);
   printf("  test_install_branch_hook: ok\n");
}

/* A non-git dir has no default branch SHA; `out` is cleared and -1 returned. */
static void test_default_branch_sha_non_git(void)
{
   make_root("nogit-sha");
   write_file("a.c", "x");
   char sha[128] = "preset";
   assert(git_resolve_default_sha(g_root, sha, sizeof(sha)) == -1);
   assert(sha[0] == '\0');
   printf("  test_default_branch_sha_non_git: ok\n");
}

int main(void)
{
   printf("test_code_collect:\n");
   /* Skip gracefully if git is unavailable in the test environment. */
   if (system("git --version >/dev/null 2>&1") != 0)
   {
      printf("  (git unavailable; skipping)\nALL PASS\n");
      return 0;
   }
   test_default_branch_is_canonical();
   test_worktree_optin();
   test_clone_resolves_origin_head();
   test_no_default_branch_skips();
   test_non_git_uses_worktree();
   test_build_manifests_collected();
   test_build_manifests_collected_git();
   test_default_branch_sha_tracks_commits();
   test_default_branch_sha_quote_in_ref();
   test_index_source_is_worktree();
   test_install_branch_hook();
   test_default_branch_sha_non_git();
   sh("rm -rf '%s'", g_root);
   printf("ALL PASS\n");
   return 0;
}
