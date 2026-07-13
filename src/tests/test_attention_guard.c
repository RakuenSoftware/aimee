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

/* Stubs/fakes for handle_attention_guard's deps. read_stdin + aimee_home are
 * driven by the functional test below via these globals. */
static const char *g_stdin_json = NULL;
static char g_home[256] = "/tmp";

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
   char path[320];
   snprintf(path, sizeof(path), "%s/aimee.yaml", g_home);
   FILE *f = fopen(path, "wb");
   assert(f);
   if (body)
      fputs(body, f);
   fclose(f);
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
   snprintf(g_home, sizeof(g_home), "/tmp/aimee_ag_test_%d", (int)getpid());
   mkdir(g_home, 0700);
   char logpath[400], cfgpath[400];
   snprintf(logpath, sizeof(logpath), "%s/.cache/attention/agtest.json", g_home);
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", g_home);
   g_stdin_json = RAW_SCAN_HOOK;

   /* (1) Inert default: no aimee.yaml at all -> raw scans allowed (exit 0). */
   rm_path(cfgpath);
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
   rm_path(cfgpath);
   g_stdin_json = NULL;
   printf("enforcement OK\n");
}

/* Pure decision tests for the session-isolation guard. */
static void test_session_isolation_decision(void)
{
   const char *wt = "/home/u/repo/.aimee/worktrees/ab12/main/src/x.c";
   const char *primary = "/home/u/repo/src/x.c";
   const char *wt_cwd = "/home/u/repo/.aimee/worktrees/ab12/main";
   const char *primary_cwd = "/home/u/repo";

   /* Read/raw-scan ops are never blocked, regardless of location. */
   assert(attn_session_isolation_blocked(ATTN_OP_READ, primary, primary_cwd) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_RAW_SCAN, primary, primary_cwd) == 0);

   /* Mutating op with an absolute target inside a managed worktree -> allowed. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, wt, primary_cwd) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, wt, primary_cwd) == 0);

   /* Mutating op with an absolute target in the primary checkout -> BLOCKED,
    * even if cwd happens to be a worktree (escaping the worktree). */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, primary, primary_cwd) == 1);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, primary, wt_cwd) == 1);

   /* Relative / no file_path -> cwd is authoritative. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "src/x.c", wt_cwd) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "src/x.c", primary_cwd) == 1);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, NULL, wt_cwd) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, NULL, primary_cwd) == 1);

   /* Claude Code's native worktrees (/.claude/worktrees/) are equally isolated
    * branches and are honoured too (target path OR cwd). */
   const char *cc_wt = "/home/u/repo/.claude/worktrees/feat/src/x.c";
   const char *cc_wt_cwd = "/home/u/repo/.claude/worktrees/feat";
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, cc_wt, primary_cwd) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, NULL, cc_wt_cwd) == 0);
   /* The loose "/.claude" prefix (e.g. ~/.claude/) is NOT a managed worktree. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "/home/u/.claude/x.c", primary_cwd) == 1);
   /* ...but the harness's own per-project state dir (auto-memory etc.) is
    * session state, not repo content — writable from any cwd. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "/home/u/.claude/projects/p/memory/m.md",
                                         primary_cwd) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, "/home/u/.claude/projects/p/MEMORY.md",
                                         wt_cwd) == 0);

   /* Codex's native worktrees (/.codex/worktrees/) are honoured the same way. */
   const char *cx_wt = "/home/u/repo/.codex/worktrees/feat/src/x.c";
   const char *cx_wt_cwd = "/home/u/repo/.codex/worktrees/feat";
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, cx_wt, primary_cwd) == 0);
   assert(attn_session_isolation_blocked(ATTN_OP_HARD, NULL, cx_wt_cwd) == 0);
   /* The loose "/.codex" prefix is NOT a managed worktree. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "/home/u/.codex/x.c", primary_cwd) == 1);

   /* The loose "/.aimee-" prefix is NOT treated as a managed worktree (only the
    * canonical "/.aimee/worktrees/" counts) — avoids false-matching e.g. a
    * user's "/.aimee-notes" dir. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "/tmp/.aimee-xyz/src/x.c", primary_cwd) ==
          1);

   /* Path-traversal escape OUT of a worktree is blocked (lexically normalized). */
   assert(attn_session_isolation_blocked(
              ATTN_OP_SOFT, "/repo/.aimee/worktrees/x/main/../../../src/y.c", primary_cwd) == 1);
   /* '..' that stays WITHIN the worktree is still allowed. */
   assert(attn_session_isolation_blocked(
              ATTN_OP_SOFT, "/repo/.aimee/worktrees/x/main/sub/../file.c", primary_cwd) == 0);
   /* A relative target whose '..' climbs out of a worktree cwd is blocked. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, "../../../etc/x", wt_cwd) == 1);

   /* Fail-closed when both target and cwd are unknown. */
   assert(attn_session_isolation_blocked(ATTN_OP_SOFT, NULL, NULL) == 1);
   printf("isolation decision OK\n");
}

/* Functional test: the require_session_worktree gate. The handler uses the real
 * process cwd (the build dir — not a managed worktree), so an Edit with an
 * absolute file_path drives the decision deterministically. */
static void test_isolation_enforcement(void)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee_iso_test_%d", (int)getpid());
   mkdir(g_home, 0700);
   char cfgpath[400];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", g_home);

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
   rm_path(cfgpath);
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

   rm_path(cfgpath);
   g_stdin_json = NULL;
   printf("isolation enforcement OK\n");
}

int main(void)
{
   printf("attention_guard: ");
   test_classify();
   test_weight();
   test_score();
   test_guard_enforcement();
   test_session_isolation_decision();
   test_isolation_enforcement();
   printf("all tests passed\n");
   return 0;
}
