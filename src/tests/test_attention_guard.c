/* test_attention_guard.c: unit tests for the P3 attention-guard pure helpers
 * (scoring with recency decay, op classification, kind weights) plus a
 * functional test of the hook handler's raw-scan enforcement (inert by default,
 * blocking only at a positive ingress_max_raw_scans cap). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "cli_attention_guard.h"
#include "client_config.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* Stubs/fakes for handle_attention_guard's deps. read_stdin + aimee_home are
 * driven by the functional test below via these globals. */
static const char *g_stdin_json = NULL;
static char g_home[256] = "/tmp";
static int g_ingress_max_raw_scans;
static int g_require_session_worktree = 1;
static int g_require_aimee_memory = 1;

static cJSON *test_config_value(const char *key)
{
   if (!strcmp(key, "ingress_max_raw_scans"))
      return cJSON_CreateNumber(g_ingress_max_raw_scans);
   if (!strcmp(key, "require_session_worktree"))
      return cJSON_CreateBool(g_require_session_worktree);
   if (!strcmp(key, "require_aimee_memory"))
      return cJSON_CreateBool(g_require_aimee_memory);
   return NULL;
}

char *read_stdin(void)
{
   return g_stdin_json ? strdup(g_stdin_json) : NULL;
}
const char *aimee_home(void)
{
   return g_home;
}
int platform_mkdir_p(const char *path, int mode)
{
   /* Real recursive mkdir so the handler can persist its per-session raw-scan
    * log (the cap test depends on that count surviving across invocations). */
   char buf[512];
   snprintf(buf, sizeof(buf), "%s", path);
   for (char *p = buf + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         mkdir(buf, (mode_t)mode);
         *p = '/';
      }
   }
   mkdir(buf, (mode_t)mode);
   return 0;
}

static void test_classify(void)
{
   assert(attn_classify("Read", NULL) == ATTN_OP_READ);
   assert(attn_classify("Edit", NULL) == ATTN_OP_SOFT);
   assert(attn_classify("Write", NULL) == ATTN_OP_SOFT);
   assert(attn_classify("MultiEdit", NULL) == ATTN_OP_SOFT);
   assert(attn_classify("NotebookEdit", NULL) == ATTN_OP_SOFT);
   assert(attn_classify("Bash", "rm -rf src/x.c") == ATTN_OP_HARD);
   assert(attn_classify("Bash", "rm -fr build") == ATTN_OP_HARD);
   assert(attn_classify("Bash", "truncate -s0 log") == ATTN_OP_HARD);
   assert(attn_classify("Bash", "shred secret") == ATTN_OP_HARD);
   assert(attn_classify("Bash", ": > file") == ATTN_OP_HARD);
   assert(attn_classify("Bash", "grep -R symbol .") == ATTN_OP_RAW_SCAN);
   assert(attn_classify("Bash", "rg --files") == ATTN_OP_RAW_SCAN);
   assert(attn_classify("Bash", "find . -name '*.c'") == ATTN_OP_RAW_SCAN);
   assert(attn_classify("Bash", "rm stale.txt") == ATTN_OP_SOFT);  /* non-recursive */
   assert(attn_classify("Bash", "echo hi > out") == ATTN_OP_SOFT); /* redirect overwrite */
   assert(attn_classify("Bash", "ls -la") == ATTN_OP_READ);
   assert(attn_classify("Grep", NULL) == ATTN_OP_RAW_SCAN);
   assert(attn_classify("Glob", NULL) == ATTN_OP_RAW_SCAN);
   assert(attn_classify(NULL, NULL) == ATTN_OP_READ);
   assert(attn_is_raw_scan("Bash", "grep -r TODO src") == 1);
   assert(attn_is_raw_scan("Bash", "grep TODO src/file.c") == 0);
   printf("classify OK\n");
}

static void test_weight(void)
{
   assert(attn_weight_for(ATTN_OP_READ) == 2);
   assert(attn_weight_for(ATTN_OP_SOFT) == 8);
   assert(attn_weight_for(ATTN_OP_HARD) == 8);
   printf("weight OK\n");
}

static void test_score(void)
{
   long now = 1000000;
   attn_record_t recs[] = {
       {"a.c", 8, now},           /* fresh edit */
       {"b.c", 2, now},           /* fresh read */
       {"a.c", 2, now - 3600},    /* a read 1h ago -> decays to 1.0 */
       {"old.c", 8, now - 36000}, /* 10h ago -> 8 * 2^-10 ~ 0.0078 */
   };
   int n = (int)(sizeof(recs) / sizeof(recs[0]));

   /* fresh edit (8) + 1h-old read (2*0.5=1) = 9.0 */
   double a = attn_score(recs, n, "a.c", now);
   assert(fabs(a - 9.0) < 0.001);
   /* fresh read = 2.0 == threshold (high attention) */
   assert(fabs(attn_score(recs, n, "b.c", now) - 2.0) < 0.001);
   assert(attn_score(recs, n, "b.c", now) >= ATTN_HIGH_THRESHOLD);
   /* a 10h-old single edit is well below threshold */
   assert(attn_score(recs, n, "old.c", now) < ATTN_HIGH_THRESHOLD);
   /* unknown path -> 0 */
   assert(attn_score(recs, n, "missing.c", now) == 0.0);
   /* NULL safety */
   assert(attn_score(NULL, 0, "x", now) == 0.0);
   printf("score OK\n");
}

/* Hook input for a recursive raw scan (the Bash `grep -r` form). */
#define RAW_SCAN_HOOK                                                                              \
   "{\"session_id\":\"agtest\",\"tool_name\":\"Bash\","                                            \
   "\"tool_input\":{\"command\":\"grep -r TODO src\"}}"

static void write_config(const char *body)
{
   g_ingress_max_raw_scans = body && strstr(body, "ingress_max_raw_scans: 2") ? 2 : 0;
   g_require_session_worktree =
       !(body && strstr(body, "require_session_worktree: false"));
   g_require_aimee_memory = !(body && strstr(body, "require_aimee_memory: false"));
}

static void rm_path(const char *p)
{
   remove(p);
}

/* Functional test of the raw-scan enforcement: inert unless a positive
 * ingress_max_raw_scans cap is configured. */
static void test_guard_enforcement(void)
{
   /* Isolated, real temp home so config + the session log persist. */
   snprintf(g_home, sizeof(g_home), "%s/aimee_ag_test_%d", platform_tmpdir(), (int)getpid());
   mkdir(g_home, 0700);
   char logpath[400];
   snprintf(logpath, sizeof(logpath), "%s/.cache/attention/agtest.json", g_home);
   g_stdin_json = RAW_SCAN_HOOK;

   /* (1) Inert default: no aimee.yaml at all -> raw scans allowed (exit 0). */
   write_config(NULL);
   rm_path(logpath);
   assert(handle_attention_guard() == 0);
   assert(handle_attention_guard() == 0); /* still allowed, repeatedly */

   /* (2) Explicit 0 is also disabled. */
   write_config("ingress_max_raw_scans: 0\n");
   rm_path(logpath);
   assert(handle_attention_guard() == 0);

   /* (3) Positive cap of 2: first two scans allowed, the third is blocked. */
   write_config("ingress_max_raw_scans: 2\n");
   rm_path(logpath);
   assert(handle_attention_guard() == 0); /* used 0 -> allow, count 1 */
   assert(handle_attention_guard() == 0); /* used 1 -> allow, count 2 */
   assert(handle_attention_guard() == 2); /* used 2 >= cap -> block */

   /* (4) The removed AIMEE_GUARD env bypass no longer disables the guard: an
    * agent cannot set an env var to escape the cap. Still blocked at the cap. */
   setenv("AIMEE_GUARD", "0", 1);
   assert(handle_attention_guard() == 2);
   unsetenv("AIMEE_GUARD");

   rm_path(logpath);
   write_config(NULL);
   g_stdin_json = NULL;
   printf("enforcement OK\n");
}

static void test_required_discovery_activation(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee_discovery_test_%d", platform_tmpdir(), (int)getpid());
   mkdir(g_home, 0700);
   char reason[1024];

   unsetenv("AIMEE_HOOK_TRANSPORT");
   assert(attn_discovery_gate("Bash", "ls", "legacy", reason, sizeof(reason)) == 0);

   setenv("AIMEE_HOOK_TRANSPORT", "cli", 1);
   setenv("AIMEE_CLI_PATH", "/opt/aimee/bin/aimee", 1);
   assert(attn_discovery_gate("Bash", "ls -la", "cli-session", reason, sizeof(reason)) == 2);
   assert(strstr(reason, "/opt/aimee/bin/aimee index investigate") != NULL);
   assert(attn_discovery_gate("Bash", "/opt/aimee/bin/aimee index investigate \"fix cache\"",
                              "cli-session", reason, sizeof(reason)) == 0);
   assert(attn_discovery_gate("Read", NULL, "cli-session", reason, sizeof(reason)) == 0);

   setenv("AIMEE_HOOK_TRANSPORT", "mcp", 1);
   assert(attn_discovery_gate("Read", NULL, "mcp-session", reason, sizeof(reason)) == 2);
   assert(attn_discovery_gate("mcp__aimee__index", "investigate", "mcp-session", reason,
                              sizeof(reason)) == 0);
   assert(attn_discovery_gate("Grep", NULL, "mcp-session", reason, sizeof(reason)) == 0);

   unsetenv("AIMEE_HOOK_TRANSPORT");
   unsetenv("AIMEE_CLI_PATH");
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_home);
   system(cmd);
   printf("required discovery OK\n");
}

/* The session-scratch carve-out. Real directories, because the decision is no
 * longer purely lexical: it resolves the scratch root and the target's deepest
 * existing ancestor through symlinks and requires physical containment. Every
 * negative below therefore CREATES its directories too — otherwise it would
 * pass merely because the path did not exist, which proves nothing about the
 * layout check it is meant to pin. */
static void test_session_scratch_decision(const char *primary_cwd)
{
   const char *sid = "21bc2e70-537c-4d1d-b970-7afc858a7769";
   char tmproot[256], outside[256], cmd[700];
   snprintf(tmproot, sizeof(tmproot), "%s/aimee_scratch_test_%d", platform_tmpdir(), (int)getpid());
   snprintf(outside, sizeof(outside), "%s/aimee_scratch_out_%d", platform_tmpdir(), (int)getpid());
   setenv("TMPDIR", tmproot, 1);

   char root[512], p[900];
   snprintf(root, sizeof(root), "%s/claude-1000/-home-u-repo/%s", tmproot, sid);
   snprintf(p, sizeof(p), "%s/scratchpad", root);
   platform_mkdir_p(p, 0700);
   snprintf(p, sizeof(p), "%s/tasks", root);
   platform_mkdir_p(p, 0700);
   snprintf(p, sizeof(p), "%s/repo/src", outside);
   platform_mkdir_p(p, 0700);

   /* This session's own scratch is writable — harness-owned temp space, not
    * repo content, and blocking it only pushed the same write into a shell
    * heredoc the guard does not classify as mutating. */
   snprintf(p, sizeof(p), "%s/scratchpad/notes.md", root);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, sid) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, p, primary_cwd, sid) == 0);
   /* Sibling harness dirs under the same session (task output, spills) too. */
   snprintf(p, sizeof(p), "%s/tasks/b1.output", root);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, sid) == 0);

   /* Bound to THIS session: not a general "/tmp is writable" hole, and one
    * session cannot reach into another's scratch. */
   snprintf(p, sizeof(p), "%s/scratchpad/notes.md", root);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, NULL) == 1);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, "") == 1);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, "other-session") == 1);
   snprintf(p, sizeof(p), "%s/anything.md", tmproot);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, sid) == 1);

   /* LAYOUT IS POSITIONAL, not a set of substring probes. Each of these carries
    * a "claude-" component and the session id as a whole component somewhere —
    * the shape an earlier revision of this carve-out wrongly admitted. */
   snprintf(p, sizeof(p), "%s/unrelated/claude-marker/%s/checkout", tmproot, sid);
   platform_mkdir_p(p, 0700); /* exists, so this is the layout check failing */
   snprintf(p, sizeof(p), "%s/unrelated/claude-marker/%s/checkout/x.c", tmproot, sid);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, sid) == 1);
   /* Session id in the project-slug position (reordered). */
   snprintf(p, sizeof(p), "%s/claude-1000/%s/-home-u-repo", tmproot, sid);
   platform_mkdir_p(p, 0700);
   snprintf(p, sizeof(p), "%s/claude-1000/%s/-home-u-repo/x.c", tmproot, sid);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, sid) == 1);
   /* Slug level missing entirely (too shallow). */
   snprintf(p, sizeof(p), "%s/claude-1000/%s/x.c", tmproot, sid);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, sid) == 1);
   /* A bare "claude-" with no uid suffix is not the harness dir. */
   snprintf(p, sizeof(p), "%s/claude-/-home-u-repo/%s/scratchpad", tmproot, sid);
   platform_mkdir_p(p, 0700);
   snprintf(p, sizeof(p), "%s/claude-/-home-u-repo/%s/scratchpad/x.md", tmproot, sid);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, sid) == 1);
   /* The session id must be a whole component, not a prefix match. */
   snprintf(p, sizeof(p), "%s/claude-1000/-home-u-repo/%sx", tmproot, sid);
   platform_mkdir_p(p, 0700);
   snprintf(p, sizeof(p), "%s/claude-1000/-home-u-repo/%sx/y.md", tmproot, sid);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, sid) == 1);

   /* CONTAINMENT IS PHYSICAL. A symlink inside scratch pointing at a checkout
    * is lexically inside the session dir and physically outside it; normalizing
    * "../" does not catch this. */
   char link[700];
   snprintf(link, sizeof(link), "%s/scratchpad/escape", root);
   snprintf(p, sizeof(p), "%s/repo", outside);
   remove(link);
   assert(symlink(p, link) == 0);
   snprintf(p, sizeof(p), "%s/scratchpad/escape/src/x.c", root);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, sid) == 1);

   /* THE SESSION DIRECTORY ITSELF AS A SYMLINK into the checkout. This is the
    * shape the prefix check alone cannot catch: resolving both sides lands them
    * INSIDE the checkout, so the target trivially sits under the root and the
    * carve-out would authorise writing to the repository. Only anchoring the
    * resolved root back to the expected claude-<uid>/<slug>/<session-id> shape
    * beneath the resolved temp dir rejects it. */
   {
      char linkroot[700], realrepo[700];
      snprintf(realrepo, sizeof(realrepo), "%s/repo", outside);
      snprintf(linkroot, sizeof(linkroot), "%s/claude-1000/-home-u-repo/%s", tmproot, "sid-linked");
      remove(linkroot);
      assert(symlink(realrepo, linkroot) == 0);
      snprintf(p, sizeof(p), "%s/src/x.c", linkroot);
      assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, "sid-linked") == 1);
   }

   /* A "../" escape out of the session dir stays blocked (lexical, as before). */
   snprintf(p, sizeof(p), "%s/../../../home/u/repo/src/x.c", root);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, p, primary_cwd, sid) == 1);

   /* Outside the temp root entirely, session id and claude- dir notwithstanding. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT,
                                         "/home/u/repo/claude-1000/-home-u-repo/x/src/x.c",
                                         primary_cwd, sid) == 1);

   unsetenv("TMPDIR");
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", tmproot, outside);
   (void)system(cmd);
}

/* Pure decision tests for the session-isolation guard. */
static void test_session_isolation_decision(void)
{
   const char *wt = "/home/u/repo/.aimee/worktrees/ab12/main/src/x.c";
   const char *primary = "/home/u/repo/src/x.c";
   const char *wt_cwd = "/home/u/repo/.aimee/worktrees/ab12/main";
   const char *primary_cwd = "/home/u/repo";

   /* Read/raw-scan ops are never blocked, regardless of location. */
   assert(attn_session_isolation_blocked(ATTN_OP_READ, primary, primary_cwd, NULL) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_RAW_SCAN, primary, primary_cwd, NULL) == 0);

   /* Mutating op with an absolute target inside a managed worktree -> allowed. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, wt, primary_cwd, NULL) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, wt, primary_cwd, NULL) == 0);

   /* Mutating op with an absolute target in the primary checkout -> BLOCKED,
    * even if cwd happens to be a worktree (escaping the worktree). */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, primary, primary_cwd, NULL) == 1);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, primary, wt_cwd, NULL) == 1);

   /* ---- branch lineage (attn_session_branch_blocked) ----
    * The path check alone was the bug: a hand-made worktree under .claude/worktrees/
    * passed it while sitting on a branch cut from another session's commit. */

   /* Primary rooted on the default branch: allowed, in both spellings. */
   assert(attn_session_branch_blocked("testing", "testing", 0) == 0);
   assert(attn_session_branch_blocked("origin/testing", "testing", 0) == 0);
   assert(attn_session_branch_blocked("testing", "origin/testing", 0) == 0);

   /* Delegate rooted on a REGISTERED parent session branch: allowed. */
   assert(attn_session_branch_blocked("aimee/session/parent-abc", "testing", 1) == 0);

   /* The bug: rooted on another session's branch that is NOT its parent. */
   assert(attn_session_branch_blocked("aimee/session/someone-else", "testing", 0) == 1);

   /* Unregistered worktree -- no launcher row, so hand-rolled. Fail closed. */
   assert(attn_session_branch_blocked("", "testing", 0) == 1);
   assert(attn_session_branch_blocked(NULL, "testing", 0) == 1);

   /* Unresolvable default branch must not silently allow an arbitrary base. */
   assert(attn_session_branch_blocked("aimee/session/whatever", "", 0) == 1);
   /* ...but a registered parent is still a legitimate delegate. */
   assert(attn_session_branch_blocked("aimee/session/whatever", "", 1) == 0);

   /* ---- no registry row (attn_unregistered_lineage_blocked) ----
    * THE REGRESSION: rows are written only by workspace.c, but the refusal names Claude
    * Code's EnterWorktree as sanctioned too, and that writes none. Every such session was
    * refused every mutating op -- while correctly cut from the default branch. It was
    * also unrecoverable: settings, the binary and aimee.yaml all live outside a managed
    * worktree, so this same guard refused every route to switching it off. */

   /* Clean lineage, no row -> ALLOWED. */
   assert(attn_unregistered_lineage_blocked(1, 0) == 0);

   /* Still blocked when it carries another session's unmerged work -- the incident the
    * lineage rule was added for. Losing this would defeat the whole check. */
   assert(attn_unregistered_lineage_blocked(1, 1) == 1);

   /* Default branch unresolvable: nothing to measure against, so fail closed, matching
    * attn_session_branch_blocked's behaviour with an empty default above. */
   assert(attn_unregistered_lineage_blocked(0, 0) == 1);
   assert(attn_unregistered_lineage_blocked(0, 1) == 1);

   /* ---- git probe directory (attn_git_dir_for) ----
    * THE REGRESSION: the probe directory was derived by stripping ONE component off the
    * target. For a new file in a not-yet-created directory that yields another missing
    * path, so `git -C` fails and BOTH lineage probes come back empty. The caller reads
    * default_resolved == 0 and fails closed (asserted just above) — so creating a file in
    * a new subdirectory was refused with a branch-lineage error that had nothing to do
    * with the branch, for every session without a registry row. */
   {
      char tmpl[256];
      snprintf(tmpl, sizeof tmpl, "%s/aimee_attn_gitdir_XXXXXX", platform_tmpdir());
      const char *root = mkdtemp(tmpl);
      assert(root != NULL);

      char got[2048];

      /* An existing directory is returned as-is. */
      attn_git_dir_for(root, got, sizeof(got));
      assert(strcmp(got, root) == 0);

      /* An existing FILE resolves to its (existing) parent. */
      char existing_file[2200];
      snprintf(existing_file, sizeof(existing_file), "%s/present.txt", root);
      FILE *f = fopen(existing_file, "w");
      assert(f != NULL);
      fclose(f);
      attn_git_dir_for(existing_file, got, sizeof(got));
      assert(strcmp(got, root) == 0);

      /* A new file in a MISSING directory must still resolve to a real directory —
       * one level of absence... */
      char one_deep[2200];
      snprintf(one_deep, sizeof(one_deep), "%s/newdir/corpus.json", root);
      attn_git_dir_for(one_deep, got, sizeof(got));
      assert(strcmp(got, root) == 0);

      /* ...and several. A single strip returned "<root>/a/b/c", which does not exist. */
      char deep[2400];
      snprintf(deep, sizeof(deep), "%s/a/b/c/corpus.json", root);
      attn_git_dir_for(deep, got, sizeof(got));
      assert(strcmp(got, root) == 0);

      unlink(existing_file);
      rmdir(root);
   }

   /* ---- Bash reaching outside the worktree (attn_bash_escapes_worktree) ----
    * The observed bypass: cwd was a valid managed worktree, so the isolation check
    * passed, while the command cd'd to the shared checkout and wrote there. */
   {
      const char *wt = "/home/u/repo/.claude/worktrees/w1";
      /* THE BUG, verbatim in shape. */
      assert(attn_bash_escapes_worktree("cd /home/u/repo && python3 - <<'EOF'\nx\nEOF", wt) == 1);
      assert(attn_bash_escapes_worktree("cd /home/u/repo && sed -i s/a/b/ f.c", wt) == 1);
      /* Redirect out. */
      assert(attn_bash_escapes_worktree("echo hi > /home/u/repo/src/f.c", wt) == 1);
      assert(attn_bash_escapes_worktree("tee /etc/thing < x", wt) == 1);

      /* Staying inside a managed worktree is fine, cd or redirect. */
      assert(attn_bash_escapes_worktree(
                 "cd /home/u/repo/.claude/worktrees/w1/src && sed -i s/a/b/ f.c", wt) == 0);
      assert(attn_bash_escapes_worktree("echo hi > /home/u/repo/.aimee/worktrees/s1/main/f.c",
                                        wt) == 0);
      /* Relative work never leaves by construction. */
      assert(attn_bash_escapes_worktree("cd src && sed -i s/a/b/ f.c", wt) == 0);

      /* Read-only commands are untouched even when they name outside paths. */
      assert(attn_bash_escapes_worktree("cat /etc/hosts", wt) == 0);
      assert(attn_bash_escapes_worktree("cd /home/u/repo && git log", wt) == 0);

      /* Noise that must not false-positive: fd dups, /dev/null, a bare "cd". */
      assert(attn_bash_escapes_worktree("make 2>&1 | tee out.log", wt) == 0);
      assert(attn_bash_escapes_worktree("rm -f x > /dev/null 2>&1", wt) == 0);
      /* "cd" as a substring of another word is not a chdir. */
      assert(attn_bash_escapes_worktree("rm -f /tmp/abcd /x", wt) == 1);
      assert(attn_bash_escapes_worktree("echo included > out.txt", wt) == 0);
   }

   /* Relative / no file_path -> cwd is authoritative. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "src/x.c", wt_cwd, NULL) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "src/x.c", primary_cwd, NULL) == 1);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, NULL, wt_cwd, NULL) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, NULL, primary_cwd, NULL) == 1);

   /* Claude Code's native worktrees (/.claude/worktrees/) are equally isolated
    * branches and are honoured too (target path OR cwd). */
   const char *cc_wt = "/home/u/repo/.claude/worktrees/feat/src/x.c";
   const char *cc_wt_cwd = "/home/u/repo/.claude/worktrees/feat";
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, cc_wt, primary_cwd, NULL) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, NULL, cc_wt_cwd, NULL) == 0);
   /* The loose "/.claude" prefix (e.g. ~/.claude/) is NOT a managed worktree. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "/home/u/.claude/x.c", primary_cwd, NULL) ==
          1);
   /* ...but the harness's own per-project state dir (auto-memory etc.) is
    * session state, not repo content — writable from any cwd. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "/home/u/.claude/projects/p/memory/m.md",
                                         primary_cwd, NULL) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, "/home/u/.claude/projects/p/MEMORY.md",
                                         wt_cwd, NULL) == 0);

   /* Codex's native worktrees (/.codex/worktrees/) are honoured the same way. */
   const char *cx_wt = "/home/u/repo/.codex/worktrees/feat/src/x.c";
   const char *cx_wt_cwd = "/home/u/repo/.codex/worktrees/feat";
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, cx_wt, primary_cwd, NULL) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, NULL, cx_wt_cwd, NULL) == 0);
   /* The loose "/.codex" prefix is NOT a managed worktree. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "/home/u/.codex/x.c", primary_cwd, NULL) ==
          1);

   /* The loose "/.aimee-" prefix is NOT treated as a managed worktree (only the
    * canonical "/.aimee/worktrees/" counts) — avoids false-matching e.g. a
    * user's "/.aimee-notes" dir. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "/tmp/.aimee-xyz/src/x.c", primary_cwd,
                                         NULL) == 1);

   /* Path-traversal escape OUT of a worktree is blocked (lexically normalized). */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT,
                                         "/repo/.aimee/worktrees/x/main/../../../src/y.c",
                                         primary_cwd, NULL) == 1);
   /* '..' that stays WITHIN the worktree is still allowed. */
   assert(attn_session_isolation_blocked(
              ATTN_OP_SOFT, "/repo/.aimee/worktrees/x/main/sub/../file.c", primary_cwd, NULL) == 0);
   /* A relative target whose '..' climbs out of a worktree cwd is blocked. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "../../../etc/x", wt_cwd, NULL) == 1);

   /* Fail-closed when both target and cwd are unknown. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, NULL, NULL, NULL) == 1);

   test_session_scratch_decision(primary_cwd);
   printf("isolation decision OK\n");
}

/* Pure decision tests for the external-memory guard. */
static void test_external_memory_decision(void)
{
   const char *mem = "/home/u/.claude/projects/-home-u-repo/memory/fact.md";
   const char *mem_idx = "/home/u/.claude/projects/-home-u-repo/memory/MEMORY.md";
   const char *state = "/home/u/.claude/projects/-home-u-repo/transcript.jsonl";
   const char *cwd = "/home/u/repo/.aimee/worktrees/ab/main";

   /* Mutating file tools targeting the memory store -> BLOCKED, even from a
    * managed-worktree cwd (the store is outside the repo entirely). */
   assert(attn_external_memory_blocked(ATTN_OP_SOFT, "Write", mem, NULL, cwd) == 1);
   assert(attn_external_memory_blocked(ATTN_OP_SOFT, "Edit", mem_idx, NULL, cwd) == 1);
   assert(attn_external_memory_blocked(ATTN_OP_HARD, "Bash", NULL,
                                       "rm -rf /home/u/.claude/projects/p/memory", cwd) == 1);

   /* Reads never block. */
   assert(attn_external_memory_blocked(ATTN_OP_READ, "Read", mem, NULL, cwd) == 0);
   assert(attn_external_memory_blocked(ATTN_OP_READ, "Bash", NULL,
                                       "grep -n hook /home/u/.claude/projects/p/memory/MEMORY.md",
                                       cwd) == 0);
   /* A discard-only redirect is still a read. */
   assert(attn_external_memory_blocked(ATTN_OP_READ, "Bash", NULL,
                                       "cat /home/u/.claude/projects/p/memory/m.md 2>/dev/null",
                                       cwd) == 0);

   /* Other harness state (transcripts etc.) is NOT the memory store. */
   assert(attn_external_memory_blocked(ATTN_OP_SOFT, "Write", state, NULL, cwd) == 0);
   /* ...nor is an unrelated dir that merely ends in /memory elsewhere. */
   assert(attn_external_memory_blocked(ATTN_OP_SOFT, "Write", "/home/u/repo/src/memory/x.c", NULL,
                                       cwd) == 0);

   /* The observed Bash bypasses are all caught: heredoc/cat redirect, sed -i,
    * an interpreter, and an append. */
   assert(attn_external_memory_blocked(
              ATTN_OP_READ, "Bash", NULL,
              "MEMDIR=/home/u/.claude/projects/p/memory; cat > \"$MEMDIR/x.md\" <<'EOF'\nhi\nEOF",
              cwd) == 1);
   assert(attn_external_memory_blocked(
              ATTN_OP_READ, "Bash", NULL,
              "sed -i 's/a/b/' /home/u/.claude/projects/p/memory/MEMORY.md", cwd) == 1);
   assert(attn_external_memory_blocked(
              ATTN_OP_READ, "Bash", NULL,
              "python3 - <<'EOF'\nopen('/home/u/.claude/projects/p/memory/m.md','w')\nEOF",
              cwd) == 1);
   assert(attn_external_memory_blocked(
              ATTN_OP_SOFT, "Bash", NULL,
              "echo '- hook' >> /home/u/.claude/projects/p/memory/MEMORY.md", cwd) == 1);

   /* A '..' traversal into the store is normalized and blocked. */
   assert(attn_external_memory_blocked(ATTN_OP_SOFT, "Write",
                                       "/home/u/.claude/projects/p/logs/../memory/m.md", NULL,
                                       cwd) == 1);

   /* NULL safety. */
   assert(attn_external_memory_blocked(ATTN_OP_SOFT, "Write", NULL, NULL, NULL) == 0);
   assert(attn_external_memory_blocked(ATTN_OP_SOFT, "Bash", NULL, NULL, NULL) == 0);
   printf("external-memory decision OK\n");
}

/* Functional test: the require_aimee_memory gate (default ON, explicit false
 * opts out, no env-var bypass). */
static void test_external_memory_enforcement(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee_mem_test_%d", platform_tmpdir(), (int)getpid());
   mkdir(g_home, 0700);

#define WRITE_MEMORY_HOOK                                                                          \
   "{\"session_id\":\"memtest\",\"tool_name\":\"Write\",\"tool_input\":{\"file_path\":"            \
   "\"/home/u/.claude/projects/p/memory/fact.md\"}}"
#define BASH_MEMORY_HOOK                                                                           \
   "{\"session_id\":\"memtest\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":"               \
   "\"sed -i 's/a/b/' /home/u/.claude/projects/p/memory/MEMORY.md\"}}"
#define READ_MEMORY_HOOK                                                                           \
   "{\"session_id\":\"memtest\",\"tool_name\":\"Read\",\"tool_input\":{\"file_path\":"             \
   "\"/home/u/.claude/projects/p/memory/fact.md\"}}"

   /* (1) Default ON: no config -> a Write into the store is BLOCKED. */
   write_config(NULL);
   g_stdin_json = WRITE_MEMORY_HOOK;
   assert(handle_attention_guard() == 2);

   /* (2) The Bash workaround is BLOCKED too. */
   g_stdin_json = BASH_MEMORY_HOOK;
   assert(handle_attention_guard() == 2);

   /* (3) Reading the store stays allowed (worktree isolation off so only the
    *     memory guard is in play). */
   write_config("require_session_worktree: false\n");
   g_stdin_json = READ_MEMORY_HOOK;
   assert(handle_attention_guard() == 0);

   /* (4) Explicit opt-out re-opens the store (the harness-state carve-out then
    *     applies to the file tool; the Bash text check is off too). */
   write_config("require_aimee_memory: false\nrequire_session_worktree: false\n");
   g_stdin_json = WRITE_MEMORY_HOOK;
   assert(handle_attention_guard() == 0);
   g_stdin_json = BASH_MEMORY_HOOK;
   assert(handle_attention_guard() == 0);

   /* (5) No env-var bypass. */
   write_config("require_session_worktree: false\n");
   setenv("AIMEE_GUARD", "0", 1);
   g_stdin_json = WRITE_MEMORY_HOOK;
   assert(handle_attention_guard() == 2);
   unsetenv("AIMEE_GUARD");

   write_config(NULL);
   g_stdin_json = NULL;
   printf("external-memory enforcement OK\n");
}

/* Functional test: the require_session_worktree gate. The handler uses the real
 * process cwd (the build dir — not a managed worktree), so an Edit with an
 * absolute file_path drives the decision deterministically. */
/* Runs handle_attention_guard with stderr redirected into `out`, so a test can
 * assert on the refusal diagnostic itself. Returns the handler's exit code. */
static int capture_stderr(char *out, size_t outsz)
{
   char tmp[256];
   snprintf(tmp, sizeof(tmp), "%s/aimee_attn_stderr_%d", platform_tmpdir(), (int)getpid());
   int saved = dup(STDERR_FILENO);
   assert(saved >= 0);
   FILE *f = fopen(tmp, "w+");
   assert(f != NULL);
   fflush(stderr);
   assert(dup2(fileno(f), STDERR_FILENO) >= 0);

   int rc = handle_attention_guard();

   fflush(stderr);
   assert(dup2(saved, STDERR_FILENO) >= 0);
   close(saved);
   rewind(f);
   size_t n = fread(out, 1, outsz - 1, f);
   out[n] = '\0';
   fclose(f);
   unlink(tmp);
   return rc;
}

static void test_isolation_enforcement(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee_iso_test_%d", platform_tmpdir(), (int)getpid());
   mkdir(g_home, 0700);

#define EDIT_PRIMARY_HOOK                                                                          \
   "{\"session_id\":\"isotest\",\"tool_name\":\"Edit\","                                           \
   "\"tool_input\":{\"file_path\":\"/home/u/repo/src/x.c\"}}"
#define EDIT_WORKTREE_HOOK                                                                         \
   "{\"session_id\":\"isotest\",\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":"             \
   "\"/home/u/repo/.aimee/worktrees/ab/main/src/x.c\"}}"
#define READ_PRIMARY_HOOK                                                                          \
   "{\"session_id\":\"isotest\",\"tool_name\":\"Read\","                                           \
   "\"tool_input\":{\"file_path\":\"/home/u/repo/src/x.c\"}}"

   /* (1) Default ON: with no config, a mutating op on the primary checkout is
    *     BLOCKED — session-worktree isolation is required by default so two aimee
    *     sessions cannot collide on one shared git HEAD. */
   write_config(NULL);
   g_stdin_json = EDIT_PRIMARY_HOOK;
   assert(handle_attention_guard() == 2);

   /* (2) Explicit false disables the guard (opt-out). */
   write_config("require_session_worktree: false\n");
   assert(handle_attention_guard() == 0);

   /* (3) Enabled: an Edit on the primary checkout is BLOCKED. */
   write_config("require_session_worktree: true\n");
   assert(handle_attention_guard() == 2);

   /* (4) Enabled: an Edit whose target is inside a managed worktree is allowed. */
   g_stdin_json = EDIT_WORKTREE_HOOK;
   assert(handle_attention_guard() == 0);

   /* (5) Enabled: a Read on the primary checkout is allowed (non-mutating). */
   g_stdin_json = READ_PRIMARY_HOOK;
   assert(handle_attention_guard() == 0);

   /* (6) The AIMEE_GUARD env bypass was removed: setting it does NOT disable the
    * guard, so an agent cannot escape isolation via an env var. Still blocked. */
   setenv("AIMEE_GUARD", "0", 1);
   g_stdin_json = EDIT_PRIMARY_HOOK;
   assert(handle_attention_guard() == 2);
   unsetenv("AIMEE_GUARD");

   /* (7) The refusal must name the path it JUDGED. With an absolute file_path
    *     the effective target is that file — and the cwd can be a perfectly
    *     good managed worktree. Reporting the cwd as "not a managed worktree"
    *     there is a false statement that sends the reader diagnosing the wrong
    *     thing (observed: a whole session lost to it). */
   char wtcwd[512];
   snprintf(wtcwd, sizeof(wtcwd), "%s/repo/.aimee/worktrees/ab/main", g_home);
   platform_mkdir_p(wtcwd, 0700);
   char prev_cwd[512];
   assert(getcwd(prev_cwd, sizeof(prev_cwd)) != NULL);
   assert(chdir(wtcwd) == 0);

   g_stdin_json = EDIT_PRIMARY_HOOK; /* target /home/u/repo/src/x.c, cwd IS managed */
   char msg[4096];
   assert(capture_stderr(&msg[0], sizeof(msg)) == 2);
   assert(strstr(msg, "/home/u/repo/src/x.c") != NULL); /* names the real offender */
   assert(strstr(msg, wtcwd) == NULL);                  /* does not accuse the good cwd */

   /* A Bash mutation has no file_path, so the cwd IS the judged target and
    * naming it is correct. Same cwd, and now it is allowed — which is exactly
    * why claiming it was unmanaged above was wrong. */
   g_stdin_json = "{\"session_id\":\"isotest\",\"tool_name\":\"Bash\","
                  "\"tool_input\":{\"command\":\"rm -rf build\"}}";
   assert(handle_attention_guard() == 0);

   assert(chdir(prev_cwd) == 0);

   write_config(NULL);
   g_stdin_json = NULL;
   printf("isolation enforcement OK\n");
}

int main(void)
{
   client_config_set_provider(test_config_value);
   printf("attention_guard: ");
   test_classify();
   test_weight();
   test_score();
   test_guard_enforcement();
   test_required_discovery_activation();
   test_session_isolation_decision();
   test_isolation_enforcement();
   test_external_memory_decision();
   test_external_memory_enforcement();
   printf("all tests passed\n");
   return 0;
}
