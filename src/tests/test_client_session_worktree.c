/* test_client_session_worktree.c: the thin-client per-session branch+worktree
 * bootstrap shared by the SessionStart hook and the MCP proxy.
 *
 * The behavior under test is a git-subprocess policy, so these are not pure unit
 * tests: each case builds a throwaway repo in a temp dir and asserts against
 * real `git` output. That is the point — the bug this code exists to prevent
 * (a session silently inheriting whatever branch the shared checkout is on) is
 * invisible to a mocked git.
 */
#include "client_session_worktree.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include "platform_test_util.h"
#if defined(__linux__)
#include <sys/prctl.h>
#endif

static char g_tmp_root[512];

static int shell(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int shell(const char *fmt, ...)
{
   char cmd[4096];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(cmd, sizeof(cmd), fmt, ap);
   va_end(ap);
   return system(cmd);
}

/* First trimmed stdout line of `cmd`, or "" on failure. */
static void capture(char *out, size_t cap, const char *fmt, ...)
{
   char cmd[4096];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(cmd, sizeof(cmd), fmt, ap);
   va_end(ap);
   out[0] = '\0';
   FILE *fp = popen(cmd, "r");
   if (!fp)
      return;
   if (fgets(out, (int)cap, fp))
   {
      size_t n = strlen(out);
      while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
         out[--n] = '\0';
   }
   pclose(fp);
}

/* A repo with one commit on `branch`. Returns its path in out[cap]. */
static void make_repo(const char *name, const char *branch, char *out, size_t cap)
{
   snprintf(out, cap, "%s/%s", g_tmp_root, name);
   shell("rm -rf '%s' && mkdir -p '%s'", out, out);
   shell("git -C '%s' init -q -b '%s' >/dev/null 2>&1", out, branch);
   shell("git -C '%s' config user.email t@t && git -C '%s' config user.name t", out, out);
   shell("echo seed > '%s/seed.txt'", out);
   shell("git -C '%s' add -A >/dev/null 2>&1 && git -C '%s' commit -qm seed >/dev/null 2>&1", out,
         out);
}

/* Clone `src` so the clone has a real origin with an advertised default branch. */
static void make_clone(const char *src, const char *name, char *out, size_t cap)
{
   snprintf(out, cap, "%s/%s", g_tmp_root, name);
   shell("rm -rf '%s'", out);
   shell("git clone -q '%s' '%s' >/dev/null 2>&1", src, out);
   shell("git -C '%s' config user.email t@t && git -C '%s' config user.name t", out, out);
}

/* ---- client_session_worktree_key ---- */
static void test_key_is_collision_free(void)
{
   char a[80], b[80];
   /* Two ids sharing the same 8-char sanitized prefix must NOT collide: the key
    * carries a hash of the FULL id, so they map to different worktrees. */
   client_session_worktree_key("abcdefgh-1111-aaaa", a, sizeof a);
   client_session_worktree_key("abcdefgh-2222-bbbb", b, sizeof b);
   assert(a[0] && b[0]);
   assert(strcmp(a, b) != 0);

   /* Stable for a given id: startup/resume/compact must reuse one worktree. */
   char again[80];
   client_session_worktree_key("abcdefgh-1111-aaaa", again, sizeof again);
   assert(strcmp(a, again) == 0);

   /* Non-alnum-only ids still produce a usable (hash-only) key. */
   char punct[80];
   client_session_worktree_key("----", punct, sizeof punct);
   assert(punct[0]);

   /* No session id -> no key. */
   char empty[80];
   client_session_worktree_key("", empty, sizeof empty);
   assert(empty[0] == '\0');
   client_session_worktree_key(NULL, empty, sizeof empty);
   assert(empty[0] == '\0');
   printf("  key collision-free + stable: ok\n");
}

/* ---- client_session_worktree_base ---- */
static void test_base_prefers_remote_default(void)
{
   char upstream[512], clone[512];
   make_repo("upstream-a", "testing", upstream, sizeof upstream);
   make_clone(upstream, "clone-a", clone, sizeof clone);

   /* Put the clone on a DIFFERENT branch than the remote default. The base must
    * still be the remote default — inheriting the current branch is the bug. */
   shell("git -C '%s' checkout -q -b agent/some-other-session", clone);

   char base[192];
   assert(client_session_worktree_base(clone, base, sizeof base) == 0);
   char resolved[192], upstream_head[192];
   capture(resolved, sizeof resolved, "git -C '%s' rev-parse '%s'", clone, base);
   capture(upstream_head, sizeof upstream_head, "git -C '%s' rev-parse HEAD", upstream);
   assert(resolved[0] && strcmp(resolved, upstream_head) == 0);
   printf("  base = remote default, not the checkout's branch: ok\n");
}

static void test_base_fetches_default_tip_at_session_start(void)
{
   char upstream[512], clone[512];
   make_repo("upstream-fresh", "testing", upstream, sizeof upstream);
   make_clone(upstream, "clone-fresh", clone, sizeof clone);
   shell("printf latest > '%s/latest.txt' && git -C '%s' add latest.txt && "
         "git -C '%s' commit -qm latest",
         upstream, upstream, upstream);

   char stale[192], latest[192], base[192], resolved[192];
   capture(stale, sizeof stale, "git -C '%s' rev-parse origin/testing", clone);
   capture(latest, sizeof latest, "git -C '%s' rev-parse HEAD", upstream);
   assert(strcmp(stale, latest) != 0);
   assert(client_session_worktree_base(clone, base, sizeof base) == 0);
   capture(resolved, sizeof resolved, "git -C '%s' rev-parse '%s'", clone, base);
   assert(strcmp(resolved, latest) == 0);
   printf("  base fetches the exact default tip at session start: ok\n");
}

static void test_base_never_falls_back_to_current_branch(void)
{
   /* No remote, and no main/master to fall back to: the only branch present is
    * the current one. Resolution must FAIL rather than silently return it. */
   char repo[512];
   make_repo("no-default", "feature/only-branch", repo, sizeof repo);

   char base[192];
   assert(client_session_worktree_base(repo, base, sizeof base) == -1);
   assert(base[0] == '\0');
   printf("  unresolvable base fails instead of inheriting HEAD: ok\n");
}

static void test_base_local_default_fallback(void)
{
   /* No remote, but a local `main` exists -> that is a legitimate default. */
   char repo[512];
   make_repo("local-main", "main", repo, sizeof repo);
   shell("git -C '%s' checkout -q -b agent/work", repo);

   char base[192];
   assert(client_session_worktree_base(repo, base, sizeof base) == 0);
   assert(strcmp(base, "main") == 0);
   printf("  local default branch fallback: ok\n");
}

static void test_base_explicit_ref_override(void)
{
   char repo[512];
   make_repo("explicit", "main", repo, sizeof repo);
   shell("git -C '%s' branch release/1.x", repo);

   char base[192];
   setenv("AIMEE_SESSION_WORKTREE_BASE", "release/1.x", 1);
   assert(client_session_worktree_base(repo, base, sizeof base) == 0);
   assert(strcmp(base, "release/1.x") == 0);

   /* An explicit ref that does not exist is an operator error, NOT a hint to
    * guess something else. */
   setenv("AIMEE_SESSION_WORKTREE_BASE", "release/does-not-exist", 1);
   assert(client_session_worktree_base(repo, base, sizeof base) == -1);

   /* `current` is reachable only by explicit opt-in. */
   shell("git -C '%s' checkout -q -b offline/detached-work", repo);
   setenv("AIMEE_SESSION_WORKTREE_BASE", "current", 1);
   assert(client_session_worktree_base(repo, base, sizeof base) == 0);
   assert(strcmp(base, "offline/detached-work") == 0);

   unsetenv("AIMEE_SESSION_WORKTREE_BASE");
   printf("  explicit ref override + opt-in `current`: ok\n");
}

/* ---- client_session_worktree_ensure ---- */
static void test_ensure_creates_branch_and_worktree(void)
{
   char upstream[512], clone[512];
   make_repo("upstream-b", "testing", upstream, sizeof upstream);
   make_clone(upstream, "clone-b", clone, sizeof clone);
   /* A session started while the shared checkout sits on someone else's branch. */
   shell("git -C '%s' checkout -q -b agent/someone-elses-work", clone);
   shell("echo drift >> '%s/seed.txt' && git -C '%s' commit -aqm drift", clone, clone);

   char cwd_before[512];
   assert(getcwd(cwd_before, sizeof cwd_before));
   assert(chdir(clone) == 0);

   const char *sid = "sess-1111-2222-3333";
   char wt[4200];
   int rc = client_session_worktree_ensure(sid, wt, sizeof wt);
   /* -1 means "not applicable" — e.g. isolation disabled in this environment.
    * Skip rather than fail, so the suite is honest about what it exercised. */
   if (rc == -1)
   {
      assert(chdir(cwd_before) == 0);
      printf("  ensure: SKIPPED (session isolation not enforced here)\n");
      return;
   }
   assert(rc == 0);

   /* It lands at the agreed path... */
   char key[80];
   client_session_worktree_key(sid, key, sizeof key);
   char expect[4200];
   snprintf(expect, sizeof expect, "%s/.aimee/worktrees/%s/main", clone, key);
   assert(strcmp(wt, expect) == 0);
   struct stat st;
   assert(stat(wt, &st) == 0 && S_ISDIR(st.st_mode));

   /* The implementation is intentionally invisible to ordinary Git status.
    * Use info/exclude, not a committed .gitignore edit in the user's project. */
   char source_status[512];
   capture(source_status, sizeof source_status,
           "git -C '%s' status --porcelain=v1 --untracked-files=all", clone);
   assert(source_status[0] == '\0');
   char exclude_path[1024], exclude_entry[128];
   capture(exclude_path, sizeof exclude_path,
           "git -C '%s' rev-parse --path-format=absolute --git-path info/exclude", clone);
   capture(exclude_entry, sizeof exclude_entry, "grep -Fx '/.aimee/worktrees/' '%s'", exclude_path);
   assert(strcmp(exclude_entry, "/.aimee/worktrees/") == 0);

   /* ...on its OWN session branch... */
   char branch[192];
   capture(branch, sizeof branch, "git -C '%s' rev-parse --abbrev-ref HEAD", wt);
   char expect_branch[192];
   snprintf(expect_branch, sizeof expect_branch, "aimee/session/%s", key);
   assert(strcmp(branch, expect_branch) == 0);

   /* ...cut from the DEFAULT branch, carrying none of the other session's work. */
   char merge_base[192], origin_head[192];
   capture(merge_base, sizeof merge_base, "git -C '%s' rev-parse HEAD", wt);
   capture(origin_head, sizeof origin_head, "git -C '%s' rev-parse origin/testing", clone);
   assert(merge_base[0] && strcmp(merge_base, origin_head) == 0);

   /* Idempotent: a resume/compact re-run reuses the same worktree. */
   char wt2[4200];
   assert(client_session_worktree_ensure(sid, wt2, sizeof wt2) == 0);
   assert(strcmp(wt, wt2) == 0);

   /* A DIFFERENT session in the same repo gets a different worktree+branch. */
   char wt3[4200];
   assert(client_session_worktree_ensure("sess-9999-8888-7777", wt3, sizeof wt3) == 0);
   assert(strcmp(wt, wt3) != 0);

   assert(chdir(cwd_before) == 0);
   printf("  ensure: own branch off the default branch, idempotent, per-session: ok\n");
}

static void test_explicit_feature_base_incorporates_latest_default(void)
{
   char upstream[512], clone[512];
   make_repo("upstream-feature", "testing", upstream, sizeof upstream);
   make_clone(upstream, "clone-feature", clone, sizeof clone);
   shell("git -C '%s' checkout -qb feature/work && printf feature > '%s/feature.txt' && "
         "git -C '%s' add feature.txt && git -C '%s' commit -qm feature",
         clone, clone, clone, clone);
   char feature_oid[192];
   capture(feature_oid, sizeof feature_oid, "git -C '%s' rev-parse HEAD", clone);

   shell("printf latest > '%s/latest.txt' && git -C '%s' add latest.txt && "
         "git -C '%s' commit -qm latest",
         upstream, upstream, upstream);
   setenv("AIMEE_SESSION_WORKTREE_BASE", "feature/work", 1);
   char wt[4200];
   assert(client_session_worktree_ensure_at("feature-with-fresh-default", clone, wt, sizeof wt) ==
          0);
   unsetenv("AIMEE_SESSION_WORKTREE_BASE");

   assert(shell("git -C '%s' merge-base --is-ancestor '%s' HEAD", wt, feature_oid) == 0);
   assert(shell("git -C '%s' merge-base --is-ancestor origin/testing HEAD", wt) == 0);
   char feature_path[4300], latest_path[4300];
   snprintf(feature_path, sizeof feature_path, "%s/feature.txt", wt);
   snprintf(latest_path, sizeof latest_path, "%s/latest.txt", wt);
   assert(access(feature_path, F_OK) == 0);
   assert(access(latest_path, F_OK) == 0);
   printf("  explicit feature base incorporates freshly fetched default: ok\n");
}

static void test_concurrent_session_starts_get_distinct_worktrees(void)
{
   char upstream[512], clone[512];
   make_repo("upstream-concurrent", "testing", upstream, sizeof upstream);
   make_clone(upstream, "clone-concurrent", clone, sizeof clone);

   enum
   {
      N = 6
   };
   pid_t children[N];
   for (int i = 0; i < N; i++)
   {
      children[i] = fork();
      assert(children[i] >= 0);
      if (children[i] == 0)
      {
         char sid[64], wt[4200];
         snprintf(sid, sizeof sid, "concurrent-session-%d", i);
         _exit(client_session_worktree_ensure_at(sid, clone, wt, sizeof wt) == 0 ? 0 : 1);
      }
   }
   for (int i = 0; i < N; i++)
   {
      int status = 1;
      assert(waitpid(children[i], &status, 0) == children[i]);
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }

   char paths[N][4200];
   for (int i = 0; i < N; i++)
   {
      char sid[64], key[80];
      snprintf(sid, sizeof sid, "concurrent-session-%d", i);
      client_session_worktree_key(sid, key, sizeof key);
      snprintf(paths[i], sizeof paths[i], "%s/.aimee/worktrees/%s/main", clone, key);
      struct stat st;
      assert(stat(paths[i], &st) == 0 && S_ISDIR(st.st_mode));
      for (int j = 0; j < i; j++)
         assert(strcmp(paths[i], paths[j]) != 0);
   }
   printf("  concurrent starts serialize and receive distinct worktrees: ok\n");
}

static void test_ensure_requires_a_session_id_and_a_repo(void)
{
   char repo[512];
   make_repo("no-sid", "main", repo, sizeof repo);
   char cwd_before[512];
   assert(getcwd(cwd_before, sizeof cwd_before));

   char wt[4200];
   assert(chdir(repo) == 0);
   assert(client_session_worktree_ensure(NULL, wt, sizeof wt) == -1);
   assert(client_session_worktree_ensure("", wt, sizeof wt) == -1);
   assert(wt[0] == '\0');

   /* Outside a git repo there is nothing to branch from. */
   char bare[512];
   snprintf(bare, sizeof bare, "%s/not-a-repo", g_tmp_root);
   shell("rm -rf '%s' && mkdir -p '%s'", bare, bare);
   assert(chdir(bare) == 0);
   assert(client_session_worktree_ensure("sess-abc", wt, sizeof wt) == -1);

   assert(chdir(cwd_before) == 0);
   printf("  ensure: no session id / no repo -> not applicable: ok\n");
}

static void test_routes_reads_writes_shell_and_patch_per_session(void)
{
   char upstream[512], clone[512];
   make_repo("upstream-route", "testing", upstream, sizeof upstream);
   make_clone(upstream, "clone-route", clone, sizeof clone);
   shell("mkdir -p '%s/src'", clone);

   const char *a = "route-session-a";
   const char *b = "route-session-b";
   char wt_a[4200], wt_b[4200];
   assert(client_session_worktree_ensure_at(a, clone, wt_a, sizeof wt_a) == 0);
   assert(client_session_worktree_ensure_at(b, clone, wt_b, sizeof wt_b) == 0);
   assert(strcmp(wt_a, wt_b) != 0);

   char routed[32768], expect[4200];
   assert(client_session_worktree_route_path(a, clone, "src/new.c", routed, sizeof routed) == 0);
   snprintf(expect, sizeof expect, "%s/src/new.c", wt_a);
   assert(strcmp(routed, expect) == 0);

   /* Once a host adopts the session checkout as cwd, model-generated absolute
    * paths may still name the original checkout. They must remain isolated. */
   char source_seed[4200];
   snprintf(source_seed, sizeof source_seed, "%s/seed.txt", clone);
   assert(client_session_worktree_route_path(a, wt_a, source_seed, routed, sizeof routed) == 0);
   snprintf(expect, sizeof expect, "%s/seed.txt", wt_a);
   assert(strcmp(routed, expect) == 0);
   assert(client_session_worktree_route_path(a, clone, expect, routed, sizeof routed) == 0);
   assert(strcmp(routed, expect) == 0); /* repeated host callbacks are idempotent */
   char source_cmd[8400];
   snprintf(source_cmd, sizeof source_cmd, "printf adopted > '%s/adopted.txt'", clone);
   assert(client_session_worktree_route_command(a, wt_a, source_cmd, routed, sizeof routed) == 0);
   char expected_cmd[8400];
   snprintf(expected_cmd, sizeof expected_cmd, "printf adopted > '%s/adopted.txt'", wt_a);
   assert(strcmp(routed, expected_cmd) == 0);

   /* Explicit access to B's tree from A is never accepted merely because both
    * paths contain the managed-worktree marker. */
   snprintf(expect, sizeof expect, "%s/seed.txt", wt_b);
   assert(client_session_worktree_route_path(a, clone, expect, routed, sizeof routed) == -3);
   assert(client_session_worktree_route_command(a, clone, expect, routed, sizeof routed) == -3);
   assert(client_session_worktree_route_path(a, clone, "/tmp/.claude/worktrees/another/src/x.c",
                                             routed, sizeof routed) == -3);
   assert(client_session_worktree_route_command(
              a, clone, "cat /tmp/.codex/worktrees/another/src/x.c", routed, sizeof routed) == -3);

   assert(client_session_worktree_route_command(a, clone, "printf routed > shell.txt", routed,
                                                sizeof routed) == 0);
   assert(strstr(routed, wt_a) != NULL);
   char routed_again[32768];
   assert(client_session_worktree_route_command(a, clone, routed, routed_again,
                                                sizeof routed_again) == 0);
   assert(strcmp(routed, routed_again) == 0);
   assert(shell("%s", routed) == 0);
   snprintf(expect, sizeof expect, "%s/shell.txt", wt_a);
   assert(access(expect, F_OK) == 0);
   snprintf(expect, sizeof expect, "%s/shell.txt", clone);
   assert(access(expect, F_OK) != 0); /* shared checkout stayed untouched */

   const char *patch = "*** Begin Patch\n*** Add File: src/from-patch.c\n+x\n*** End Patch\n";
   assert(client_session_worktree_route_patch(a, clone, patch, routed, sizeof routed) == 0);
   char key[80];
   client_session_worktree_key(a, key, sizeof key);
   char fragment[256];
   snprintf(fragment, sizeof fragment, ".aimee/worktrees/%s/main/src/from-patch.c", key);
   assert(strstr(routed, fragment) != NULL);

   /* Starting from B's checkout still resolves A's checkout, never reuses B. */
   char from_foreign[4200];
   assert(client_session_worktree_ensure_at(a, wt_b, from_foreign, sizeof from_foreign) == 0);
   assert(strcmp(from_foreign, wt_a) == 0);
   printf("  route: reads/writes/shell/patch isolated and cross-session access blocked: ok\n");
}

static void test_release_recycles_only_clean_session_worktrees(void)
{
   char upstream[512], clone[512];
   make_repo("upstream-release", "testing", upstream, sizeof upstream);
   make_clone(upstream, "clone-release", clone, sizeof clone);

   char clean[4200], dirty[4200], marker[4300];
   assert(client_session_worktree_ensure_at("release-clean", clone, clean, sizeof clean) == 0);
   assert(client_session_worktree_ensure_at("release-dirty", clone, dirty, sizeof dirty) == 0);
   snprintf(marker, sizeof marker, "%s/unsaved.txt", dirty);
   assert(shell("printf unsaved > '%s'", marker) == 0);

   assert(client_session_worktree_release_at("release-clean", clone) == 0);
   assert(access(clean, F_OK) != 0);

   /* SessionEnd is cleanup, never data loss. git's non-force removal refuses
    * the dirty checkout and the user's file remains available. */
   assert(client_session_worktree_release_at("release-dirty", clone) == 1);
   assert(access(dirty, F_OK) == 0);
   assert(access(marker, F_OK) == 0);
   printf("  release: clean recycled, dirty retained: ok\n");
}

/* Reproduce the pre-rekey layout by hand: <root>/.aimee/worktrees/<old_key>/main
 * on branch aimee/session/<old_key>, exactly as the old truncating derivation
 * would have left it. Returns the old key in old_key[cap]. */
static void seed_pre_rekey_worktree(const char *clone, const char *sid, char *old_key, size_t cap,
                                    char *old_path, size_t path_cap)
{
   snprintf(old_key, cap, "%.16s", sid);
   snprintf(old_path, path_cap, "%s/.aimee/worktrees/%s/main", clone, old_key);
   shell("git -C '%s' worktree add -q '%s' -b 'aimee/session/%s' origin/testing >/dev/null 2>&1",
         clone, old_path, old_key);
}

/* The key derivation changed, so a session spanning the change owns a worktree
 * under the OLD key. It must be recycled automatically rather than stranded. */
static void test_ensure_reclaims_pre_rekey_worktree(void)
{
   char upstream[512], clone[512];
   make_repo("upstream-c", "testing", upstream, sizeof upstream);
   make_clone(upstream, "clone-c", clone, sizeof clone);

   const char *sid = "aimee-task-abcdef0123456789";
   char old_key[64], old_path[1024], new_key[64];
   seed_pre_rekey_worktree(clone, sid, old_key, sizeof old_key, old_path, sizeof old_path);
   client_session_worktree_key(sid, new_key, sizeof new_key);
   assert(strcmp(old_key, new_key) != 0);
   struct stat st;
   assert(stat(old_path, &st) == 0);

   char cwd_before[512];
   assert(getcwd(cwd_before, sizeof cwd_before));
   assert(chdir(clone) == 0);

   char wt[4200];
   int rc = client_session_worktree_ensure(sid, wt, sizeof wt);
   if (rc == -1)
   {
      assert(chdir(cwd_before) == 0);
      printf("  reclaim: SKIPPED (session isolation not enforced here)\n");
      return;
   }
   assert(rc == 0);

   /* The session runs on the NEW key... */
   assert(strstr(wt, new_key) != NULL);
   assert(stat(wt, &st) == 0);
   /* ...and the clean pre-rekey worktree is gone, with its branch and the
    * now-empty parent directory. */
   assert(stat(old_path, &st) != 0);
   char parent[1024];
   snprintf(parent, sizeof parent, "%s/.aimee/worktrees/%s", clone, old_key);
   assert(stat(parent, &st) != 0);
   char branch[256];
   capture(branch, sizeof branch, "git -C '%s' rev-parse --verify --quiet 'aimee/session/%s'",
           clone, old_key);
   assert(branch[0] == '\0');

   assert(chdir(cwd_before) == 0);
   printf("  reclaim: clean pre-rekey worktree recycled: ok\n");
}

/* Recycling must never become a way to lose work. */
static void test_reclaim_keeps_a_dirty_pre_rekey_worktree(void)
{
   char upstream[512], clone[512];
   make_repo("upstream-d", "testing", upstream, sizeof upstream);
   make_clone(upstream, "clone-d", clone, sizeof clone);

   const char *sid = "aimee-task-fedcba9876543210";
   char old_key[64], old_path[1024];
   seed_pre_rekey_worktree(clone, sid, old_key, sizeof old_key, old_path, sizeof old_path);
   /* Uncommitted work stranded in the old worktree. */
   shell("printf 'unsaved work' > '%s/wip.txt'", old_path);

   char cwd_before[512];
   assert(getcwd(cwd_before, sizeof cwd_before));
   assert(chdir(clone) == 0);

   char wt[4200];
   int rc = client_session_worktree_ensure(sid, wt, sizeof wt);
   if (rc == -1)
   {
      assert(chdir(cwd_before) == 0);
      printf("  reclaim (dirty): SKIPPED (session isolation not enforced here)\n");
      return;
   }
   assert(rc == 0);

   /* Kept, with the file intact. */
   struct stat st;
   assert(stat(old_path, &st) == 0);
   char wip[1024];
   snprintf(wip, sizeof wip, "%s/wip.txt", old_path);
   assert(stat(wip, &st) == 0);

   assert(chdir(cwd_before) == 0);
   printf("  reclaim: dirty pre-rekey worktree kept, not destroyed: ok\n");
}

/* Read back what client_session_id_publish wrote for `pid`, or "" if absent. */
static void read_published(const char *home, pid_t pid, char *out, size_t cap)
{
   out[0] = '\0';
   char path[4200];
   snprintf(path, sizeof path, "%s/session-ppid-%d", home, (int)pid);
   FILE *f = fopen(path, "r");
   if (!f)
      return;
   if (fgets(out, (int)cap, f))
      out[strcspn(out, "\r\n")] = '\0';
   fclose(f);
}

/* The publish half of the session-id rendezvous. Without it `aimee mcp serve`
 * finds nothing, mints its own id, and the session ends up on two worktrees. */
static void test_publish_reaches_the_parent(void)
{
   char home[4200];
   snprintf(home, sizeof home, "%s/publish-home", g_tmp_root);
   shell("mkdir -p '%s'", home);

   assert(client_session_id_publish("sess-abc-123", home) == 0);

   char got[128];
   read_published(home, getppid(), got, sizeof got);
   assert(strcmp(got, "sess-abc-123") == 0);

   /* Authoritative: the host's id replaces one a peer minted for itself, so a
    * second publish must overwrite rather than leave the stale value. */
   assert(client_session_id_publish("sess-xyz-999", home) == 0);
   read_published(home, getppid(), got, sizeof got);
   assert(strcmp(got, "sess-xyz-999") == 0);

   printf("  PASS: publish reaches the parent and overwrites\n");
}

/* A session id is an opaque host token, not a path fragment: anything that
 * could escape the filename or the file's one-line contract is refused rather
 * than written somewhere unintended. */
static void test_publish_rejects_unusable_ids(void)
{
   char home[4200];
   snprintf(home, sizeof home, "%s/publish-home-reject", g_tmp_root);
   shell("mkdir -p '%s'", home);

   assert(client_session_id_publish("../escape", home) == -1);
   assert(client_session_id_publish("has\nnewline", home) == -1);
   assert(client_session_id_publish("", home) == -1);
   assert(client_session_id_publish(NULL, home) == -1);
   assert(client_session_id_publish("fine", NULL) == -1);

   printf("  PASS: publish refuses ids it cannot name a file for\n");
}

#if defined(__linux__)
/* The walk is the whole point, and it is the part that is easy to get wrong.
 *
 * The hook is NOT a direct child of the host: its command carries an env
 * assignment, so the host runs it through a shell. Publishing only under
 * getppid() therefore names a shell that exits immediately, and `aimee mcp
 * serve` -- which reads the key named for its OWN parent, the host -- never
 * finds it. Rebuild that exact chain here: a process renamed "claude", a
 * stand-in shell beneath it, and the publisher beneath that. */
static void test_publish_walks_past_the_shell_to_the_host(void)
{
   char home[4200];
   snprintf(home, sizeof home, "%s/publish-home-walk", g_tmp_root);
   shell("mkdir -p '%s'", home);

   pid_t host = fork();
   assert(host >= 0);
   if (host == 0)
   {
      prctl(PR_SET_NAME, "claude", 0, 0, 0);
      pid_t sh = fork(); /* stands in for the shell the host spawns */
      if (sh == 0)
      {
         /* fork() INHERITS comm, so this must be renamed or it would still read
          * "claude" and the walk would stop here. The real shell is execve'd,
          * which resets it — rename to match rather than let the test pass for
          * a reason production does not have. */
         prctl(PR_SET_NAME, "sh", 0, 0, 0);
         pid_t hook = fork();
         if (hook == 0)
         {
            _exit(client_session_id_publish("host-assigned-sid", home) == 0 ? 0 : 1);
         }
         int st = 1;
         waitpid(hook, &st, 0);
         _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
      }
      int st = 1;
      waitpid(sh, &st, 0);
      _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
   }
   int status = 1;
   waitpid(host, &status, 0);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

   /* The host's key is what the proxy will ask for. */
   char got[128];
   read_published(home, host, got, sizeof got);
   assert(strcmp(got, "host-assigned-sid") == 0);

   printf("  PASS: publish walks past the shell and names the host\n");
}

/* And it must stop there. Publishing under every ancestor would eventually name
 * a terminal or a service manager and hand one session's id to an unrelated
 * one -- the collision the per-process key exists to prevent. */
static void test_publish_stops_at_the_host(void)
{
   char home[4200];
   snprintf(home, sizeof home, "%s/publish-home-stop", g_tmp_root);
   shell("mkdir -p '%s'", home);

   pid_t host = fork();
   assert(host >= 0);
   if (host == 0)
   {
      prctl(PR_SET_NAME, "claude", 0, 0, 0);
      pid_t hook = fork();
      if (hook == 0)
         _exit(client_session_id_publish("host-assigned-sid", home) == 0 ? 0 : 1);
      int st = 1;
      waitpid(hook, &st, 0);
      _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
   }
   int status = 1;
   waitpid(host, &status, 0);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

   /* This test process is the host's parent — it must NOT have been named. */
   char got[128];
   read_published(home, getpid(), got, sizeof got);
   assert(got[0] == '\0');

   printf("  PASS: publish stops at the host, not above it\n");
}
#endif /* __linux__ */

int main(void)
{
   printf("client session worktree bootstrap\n");

   snprintf(g_tmp_root, sizeof g_tmp_root, "%s/aimee-csw-test-%d", platform_tmpdir(),
            (int)getpid());
   shell("rm -rf '%s' && mkdir -p '%s'", g_tmp_root, g_tmp_root);
   /* Keep the harness's own env from steering base resolution. */
   unsetenv("AIMEE_SESSION_WORKTREE_BASE");

   test_key_is_collision_free();
   test_base_prefers_remote_default();
   test_base_fetches_default_tip_at_session_start();
   test_base_never_falls_back_to_current_branch();
   test_base_local_default_fallback();
   test_base_explicit_ref_override();
   test_ensure_creates_branch_and_worktree();
   test_explicit_feature_base_incorporates_latest_default();
   test_concurrent_session_starts_get_distinct_worktrees();
   test_ensure_requires_a_session_id_and_a_repo();
   test_routes_reads_writes_shell_and_patch_per_session();
   test_release_recycles_only_clean_session_worktrees();
   test_ensure_reclaims_pre_rekey_worktree();
   test_reclaim_keeps_a_dirty_pre_rekey_worktree();
   test_publish_reaches_the_parent();
   test_publish_rejects_unusable_ids();
#if defined(__linux__)
   test_publish_walks_past_the_shell_to_the_host();
   test_publish_stops_at_the_host();
#endif

   shell("rm -rf '%s'", g_tmp_root);
   printf("all client session worktree tests passed\n");
   return 0;
}
