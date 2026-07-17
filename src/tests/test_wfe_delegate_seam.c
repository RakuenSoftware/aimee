/* test_wfe_delegate_seam.c -- the delegate dispatch seam (author/implement/
 * document) and the forge `open` seam (pr.open), exercised through the engine.
 *
 * Phase A of full-autonomous-development: producing blocks dispatch real work
 * through registered hooks; with no provider they fail closed; tests inject
 * mocks to assert the wiring. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_blocks.h"
#include "wfe_def.h"
#include "wfe_engine.h"
#include "wfe_iface.h"
#include "wfe_store.h"

/* ---- mock delegate provider ---- */
static int g_deleg_calls;
static int g_deleg_rc; /* 0 success, -1 failure */
static char g_deleg_last_role[32];
static char g_deleg_last_delegate[32];
static char g_deleg_last_prompt[8192];
static int mock_deleg_run(const char *workdir, const char *role, const char *delegate,
                          const char *prompt, const char *artifact_path, char out_commit_sha[64],
                          char *err, size_t n)
{
   (void)workdir;
   (void)artifact_path;
   (void)err;
   (void)n;
   snprintf(g_deleg_last_prompt, sizeof g_deleg_last_prompt, "%s", prompt ? prompt : "");
   g_deleg_calls++;
   snprintf(g_deleg_last_role, sizeof g_deleg_last_role, "%s", role ? role : "");
   snprintf(g_deleg_last_delegate, sizeof g_deleg_last_delegate, "%s", delegate ? delegate : "");
   if (out_commit_sha)
      snprintf(out_commit_sha, 64, "deadbeef");
   return g_deleg_rc;
}
static const wfe_delegate_provider_t MOCK_DELEG = {mock_deleg_run};

/* ---- mock forge with open ---- */
static int g_open_calls;
static int g_open_rc; /* 0 success, -1 failure */
static char g_open_branch[64];
static char g_open_base[64]; /* the base the last pr.open targeted */
static wfe_ci_status_t f_ci(const char *r, const char *p)
{
   (void)r;
   (void)p;
   return WFE_CI_SUCCESS;
}
static int f_mergeable(const char *r, const char *p)
{
   (void)r;
   (void)p;
   return 1;
}
static int f_is_merged(const char *r, const char *p)
{
   (void)r;
   (void)p;
   return 0;
}
static wfe_merge_result_t f_merge(const char *r, const char *p)
{
   (void)r;
   (void)p;
   return WFE_MERGE_OK;
}
static int f_open(const char *repo, const char *branch, const char *base, const char *title,
                  const char *body, char out_pr_ref[128])
{
   (void)repo;
   (void)title;
   (void)body;
   g_open_calls++;
   snprintf(g_open_branch, sizeof g_open_branch, "%s", branch ? branch : "");
   snprintf(g_open_base, sizeof g_open_base, "%s", base ? base : "");
   if (out_pr_ref)
      snprintf(out_pr_ref, 128, "PR-7");
   return g_open_rc;
}
static const wfe_forge_t MOCK_FORGE = {f_ci, f_mergeable, f_is_merged, f_merge, f_open, NULL};

/* author.proposal -> pr.open (terminal). No git needed (author hashes its
 * artifact; pr.open uses the forge `open` seam). */
/* ---- mock verify provider (WP-1b implement gate) ---- */
static int g_verify_rc;      /* 0 = produced a verdict, -1 = could not run */
static char g_verdict[2048]; /* the structured verdict the gate will see */
static int mock_verify(const char *workdir, char *out, size_t n)
{
   (void)workdir;
   if (g_verify_rc != 0)
      return -1;
   snprintf(out, n, "%s", g_verdict);
   return 0;
}
static const wfe_verify_provider_t MOCK_VERIFY = {mock_verify};

/* ---- mock judge provider (PC3 adversarial gate). Scripted: g_judge_refutes[call]
 * says whether the Nth judgment (reviewer first, then skeptics) refutes. ---- */
static int g_judge_refutes[16];
static int g_judge_call;
static int g_judge_rc; /* -1 = judge could not run */
static int mock_judge(const char *workdir, const char *lens, char *out, size_t n)
{
   (void)workdir;
   (void)lens;
   if (g_judge_rc != 0)
      return -1;
   int r = (g_judge_call < 16) ? g_judge_refutes[g_judge_call] : 1;
   g_judge_call++;
   snprintf(out, n, "{\"refuted\":%s}", r ? "true" : "false");
   return 0;
}
static const wfe_judge_provider_t MOCK_JUDGE = {mock_judge};

static const char *WF = "name: ds\nstart: au\nnodes:\n"
                        "  - id: au\n    block: author.proposal\n    next: pr\n"
                        "  - id: pr\n    block: pr.open\n    in:\n      src: au.out\n";

/* base:feature -> a slice sub-PR targets the PARENT feature branch aimee/feat/<parent>. */
static const char *WF_FEAT = "name: dsf\nstart: au\nnodes:\n"
                             "  - id: au\n    block: author.proposal\n    next: pr\n"
                             "  - id: pr\n    block: pr.open\n    in:\n      src: au.out\n"
                             "    params:\n      base: feature\n";

/* base:trunk -> a final feature PR targets the repo's REAL default branch. pr.open is
 * open-only, so it may target a PROTECTED trunk (main) that merge would refuse. */
static const char *WF_TRUNK = "name: dst\nstart: au\nnodes:\n"
                              "  - id: au\n    block: author.proposal\n    next: pr\n"
                              "  - id: pr\n    block: pr.open\n    in:\n      src: au.out\n"
                              "    params:\n      base: trunk\n";

static int run_fresh(const char *suffix)
{
   char id[80] = "", err[256] = "";
   if (wfe_work_item_create("ds", "r", suffix, "interactive", id, err, sizeof err) != 0)
      return -99;
   if (wfe_engine_run(id, err, sizeof err) != 0)
      return -98;
   return 0;
}

int main(void)
{
   printf("wfe-delegate-seam: ");
   char home[] = "/tmp/wfe_ds_XXXXXX";
   assert(wfe_test_mkdtemp(home));
   char wf[160];
   snprintf(wf, sizeof wf, "%s/workflows", home);
   mkdir(wf, 0755);
   char p[256];
   snprintf(p, sizeof p, "%s/workflows/ds.yaml", home);
   FILE *f = fopen(p, "wb");
   assert(f);
   fputs(WF, f);
   fclose(f);
   snprintf(p, sizeof p, "%s/workflows/dsf.yaml", home);
   f = fopen(p, "wb");
   assert(f);
   fputs(WF_FEAT, f);
   fclose(f);
   snprintf(p, sizeof p, "%s/workflows/dst.yaml", home);
   f = fopen(p, "wb");
   assert(f);
   fputs(WF_TRUNK, f);
   fclose(f);
   setenv("AIMEE_HOME", home, 1);
   assert(db1_init(":memory:") == 0);

   wfe_reset_block_executors();
   wfe_register_default_executors();

   /* A: provider + forge.open installed -> both seams fire. */
   wfe_set_delegate_provider(&MOCK_DELEG);
   wfe_set_forge_provider(&MOCK_FORGE);
   g_deleg_calls = 0;
   g_open_calls = 0;
   g_deleg_rc = 0;
   g_open_rc = 0;
   assert(run_fresh("a") == 0);
   assert(g_deleg_calls == 1);                          /* author dispatched a delegate */
   assert(strcmp(g_deleg_last_role, "architect") == 0); /* author uses architect */
   assert(g_open_calls == 1);                           /* pr.open used the forge open seam */
   /* base-targeting: a top-level pr.open (no base param) targets the autonomous base. */
   assert(strcmp(g_open_base, wfe_autonomous_base()) == 0);

   /* A2: base:feature -> a slice sub-PR targets the PARENT feature branch. The run is
    *     linked to parent "P1" (as foreach.workflow would), so pr.open must resolve
    *     the base to aimee/feat/P1 (derived from the DB parent linkage, not an env). */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("dsf", "r", "feat-child", "interactive", id, err, sizeof err) ==
             0);
      assert(db1_work_item_set_parent(id, "P1") == 0);
      g_open_calls = 0;
      g_open_base[0] = '\0';
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(g_open_calls == 1);
      assert(strcmp(g_open_base, "aimee/feat/P1") == 0); /* base derived from parent linkage */
   }

   /* A3: base:trunk -> a final PR targets the repo's REAL default branch, resolved via
    *     AIMEE_DEFAULT_BRANCH. pr.open is OPEN-ONLY, so even a PROTECTED trunk (main --
    *     which merge would refuse) is a valid PR target: the open seam still fires and
    *     the PR is opened against it. This is the safe terminal of the default build wf. */
   {
      setenv("AIMEE_DEFAULT_BRANCH", "main", 1); /* protected: wfe_base_is_protected("main")==1 */
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("dst", "r", "trunk-pr", "interactive", id, err, sizeof err) == 0);
      g_open_calls = 0;
      g_open_base[0] = '\0';
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(g_open_calls == 1);                /* opened even though the base is protected */
      assert(strcmp(g_open_base, "main") == 0); /* targeted the resolved repo default branch */
      unsetenv("AIMEE_DEFAULT_BRANCH");
   }

   /* A3b: the trunk is NOT hardcoded to "main" -- a repo whose trunk is "master" (also
    *      protected) opens against "master". Proves base:trunk is trunk-name-agnostic. */
   {
      setenv("AIMEE_DEFAULT_BRANCH", "master", 1);
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("dst", "r", "trunk-master", "interactive", id, err, sizeof err) ==
             0);
      g_open_calls = 0;
      g_open_base[0] = '\0';
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(g_open_calls == 1);
      assert(strcmp(g_open_base, "master") == 0);
      unsetenv("AIMEE_DEFAULT_BRANCH");
   }

   /* A3c: base:trunk FAILS CLOSED when the trunk is unresolvable -- no
    *      AIMEE_DEFAULT_BRANCH and a non-git workdir (git symbolic-ref finds no
    *      origin/HEAD), so there is NO "main" guess: pr.open must NOT open a PR. */
   {
      setenv("AIMEE_WORKFLOW_REPO", home, 1); /* home is a mkdtemp dir, not a git repo */
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("dst", "r", "trunk-unresolved", "interactive", id, err,
                                  sizeof err) == 0);
      g_open_calls = 0;
      g_open_base[0] = '\0';
      (void)wfe_engine_run(id, err, sizeof err); /* run parks; we assert the forge never fired */
      assert(g_open_calls == 0);                 /* unresolved trunk -> pr.open failed closed */
      unsetenv("AIMEE_WORKFLOW_REPO");
   }

   /* B: no delegate provider installed -> author still advances (fail-open to the
    *    legacy behavior); pr.open still uses the forge. */
   wfe_set_delegate_provider(NULL);
   g_deleg_calls = 0;
   g_open_calls = 0;
   assert(run_fresh("b") == 0);
   assert(g_deleg_calls == 0); /* not called */
   assert(g_open_calls == 1);  /* forge open still fired */

   /* C: forge without an `open` method -> pr.open preserves prior advance (no
    *    crash), and the delegate seam still fires for author. */
   wfe_set_delegate_provider(&MOCK_DELEG);
   static const wfe_forge_t NO_OPEN = {f_ci, f_mergeable, f_is_merged, f_merge, NULL, NULL};
   wfe_set_forge_provider(&NO_OPEN);
   g_deleg_calls = 0;
   g_open_calls = 0;
   assert(run_fresh("c") == 0);
   assert(g_deleg_calls == 1);
   assert(g_open_calls == 0); /* open is NULL -> not called, no crash */

   /* C2: author.proposal accepts a pre-supplied, non-empty proposal even when the
    *     delegate changes nothing (dispatch fails / no-op). The proposals trigger
    *     supplies a complete, already-approved proposal (the merge IS the approval),
    *     so a "revise" delegate correctly makes no change — this must advance, NOT
    *     loop the draft to max_iters. */
   wfe_set_delegate_provider(&MOCK_DELEG);
   wfe_set_forge_provider(&MOCK_FORGE);
   {
      char pp[300];
      snprintf(pp, sizeof pp, "%s/complete-proposal.md", home);
      FILE *pf = fopen(pp, "wb");
      assert(pf);
      fputs("# A complete, already-approved proposal\n\nBody.\n", pf);
      fclose(pf);

      g_deleg_rc = -1; /* delegate reports success-but-no-change / failure */
      g_deleg_calls = 0;
      g_open_calls = 0;
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("ds", "r", pp, "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(g_deleg_calls >= 1); /* the author delegate WAS dispatched */
      assert(g_open_calls == 1);  /* author accepted the proposal and advanced to pr.open */
   }

   /* C3: the acceptance is strictly for an ALREADY-PRESENT proposal — with no
    *     artifact on disk, a failed author dispatch still loops (and the run never
    *     reaches pr.open). Guards against blindly advancing an empty proposal. */
   {
      char pp[300];
      snprintf(pp, sizeof pp, "%s/missing-proposal.md", home); /* never created */
      g_deleg_rc = -1;
      g_open_calls = 0;
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("ds", "r", pp, "interactive", id, err, sizeof err) == 0);
      (void)wfe_engine_run(id, err, sizeof err); /* author loops -> parks at max_iters */
      assert(g_open_calls == 0);                 /* never advanced past the author node */
   }

   /* C4: roundtable feedback is folded into the re-authoring prompt. A gate that
    *     requested changes persists blockers for the work item; the next author
    *     pass must carry them so it refines against the panel's objections rather
    *     than re-authoring blind. */
   {
      char pp[300];
      snprintf(pp, sizeof pp, "%s/fb-proposal.md", home);
      FILE *pf = fopen(pp, "wb");
      assert(pf);
      fputs("# proposal\n", pf);
      fclose(pf);
      g_deleg_rc = 0; /* author advances normally */
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("ds", "r", pp, "autonomous", id, err, sizeof err) == 0);
      /* the roundtable persisted these blockers on a prior request_changes */
      assert(wfe_feedback_write(id, "## qa\nNo tests specified. Add acceptance criteria.") == 0);
      g_deleg_last_prompt[0] = '\0';
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(strstr(g_deleg_last_prompt, "No tests specified") != NULL); /* blockers threaded */
      wfe_feedback_clear(id);
   }

   /* D: implement verify gate (WP-1b) — a unit advances ONLY on a top-level
    *    verdict:passed; everything else (incl. NO provider) fails closed. */
   {
      wfe_set_verify_provider(NULL);
      assert(wfe_implement_verify_ok(".") == 0); /* no gate -> FAIL CLOSED */

      wfe_set_verify_provider(&MOCK_VERIFY);
      g_verify_rc = 0;
      snprintf(g_verdict, sizeof g_verdict, "{\"schema_version\":1,\"verdict\":\"passed\"}");
      assert(wfe_implement_verify_ok(".") == 1); /* passed -> advance */

      snprintf(g_verdict, sizeof g_verdict, "{\"schema_version\":1,\"verdict\":\"failed\"}");
      assert(wfe_implement_verify_ok(".") == 0); /* failed -> block */

      snprintf(g_verdict, sizeof g_verdict, "{\"verdict\":\"unavailable\"}");
      assert(wfe_implement_verify_ok(".") == 0); /* unavailable -> fail closed */

      g_verify_rc = -1; /* gate could not run */
      assert(wfe_implement_verify_ok(".") == 0);

      g_verify_rc = 0;
      snprintf(g_verdict, sizeof g_verdict, "not json at all");
      assert(wfe_implement_verify_ok(".") == 0); /* unparseable -> fail closed */

      /* spoof: a top-level FAILED verdict whose nested step carries verdict:passed
       * must NOT flip the gate — only the TOP-LEVEL verdict counts. */
      snprintf(g_verdict, sizeof g_verdict,
               "{\"verdict\":\"failed\",\"steps\":[{\"name\":\"unit\",\"verdict\":\"passed\"}]}");
      assert(wfe_implement_verify_ok(".") == 0);

      wfe_set_verify_provider(NULL);
   }

   /* D1b: TDD RED gate — after the test author commits, a genuine red must NOT
    *      already pass. It is the inverse of the verify gate when a provider is
    *      present, and proceeds (1) with no provider (drivable). */
   {
      wfe_set_verify_provider(NULL);
      assert(wfe_tdd_red_ok(".") == 1); /* no provider -> cannot enforce -> proceed */

      wfe_set_verify_provider(&MOCK_VERIFY);
      g_verify_rc = 0;
      snprintf(g_verdict, sizeof g_verdict, "{\"schema_version\":1,\"verdict\":\"passed\"}");
      assert(wfe_tdd_red_ok(".") == 0); /* already passes -> no failing test -> loop */

      snprintf(g_verdict, sizeof g_verdict, "{\"schema_version\":1,\"verdict\":\"failed\"}");
      assert(wfe_tdd_red_ok(".") == 1); /* tests fail -> a real red -> proceed */

      g_verify_rc = -1; /* gate could not run -> red unconfirmed */
      assert(wfe_tdd_red_ok(".") == 0);

      g_verify_rc = 0;
      snprintf(g_verdict, sizeof g_verdict, "not json");
      assert(wfe_tdd_red_ok(".") == 0); /* unparseable -> red unconfirmed */

      wfe_set_verify_provider(NULL);
   }

   /* D2: adversarial gate (PC3) — reviewer + N skeptics; config-gated, majority-refute
    *     rejects, fail-closed without a provider. */
   {
      unsetenv("AIMEE_AUTONOMY_SKEPTICS");
      assert(wfe_implement_adversarial_ok(".") == 1); /* tier off -> pass */

      setenv("AIMEE_AUTONOMY_SKEPTICS", "3", 1);
      wfe_set_judge_provider(NULL);
      assert(wfe_implement_adversarial_ok(".") == 0); /* on + no provider -> fail closed */

      wfe_set_judge_provider(&MOCK_JUDGE);
      g_judge_rc = 0;
      /* reviewer accept + 0/3 skeptics refute -> accept */
      g_judge_call = 0;
      g_judge_refutes[0] = 0; /* reviewer */
      g_judge_refutes[1] = g_judge_refutes[2] = g_judge_refutes[3] = 0;
      assert(wfe_implement_adversarial_ok(".") == 1);
      /* reviewer accept + 1/3 skeptics refute (< majority 2) -> accept */
      g_judge_call = 0;
      g_judge_refutes[0] = 0;
      g_judge_refutes[1] = 1;
      g_judge_refutes[2] = g_judge_refutes[3] = 0;
      assert(wfe_implement_adversarial_ok(".") == 1);
      /* reviewer accept + 2/3 skeptics refute (>= majority) -> reject */
      g_judge_call = 0;
      g_judge_refutes[0] = 0;
      g_judge_refutes[1] = g_judge_refutes[2] = 1;
      g_judge_refutes[3] = 0;
      assert(wfe_implement_adversarial_ok(".") == 0);
      /* reviewer REFUTES -> reject regardless of skeptics */
      g_judge_call = 0;
      g_judge_refutes[0] = 1;
      assert(wfe_implement_adversarial_ok(".") == 0);
      /* even-K tie REJECTS: K=2, reviewer accept + 1/2 skeptics refute -> reject */
      setenv("AIMEE_AUTONOMY_SKEPTICS", "2", 1);
      g_judge_call = 0;
      g_judge_refutes[0] = 0; /* reviewer */
      g_judge_refutes[1] = 1; /* skeptic 1 refutes */
      g_judge_refutes[2] = 0; /* skeptic 2 accepts -> 1 of 2 = tie -> REJECT */
      assert(wfe_implement_adversarial_ok(".") == 0);
      /* K=2, 0/2 refute -> accept */
      g_judge_call = 0;
      g_judge_refutes[0] = g_judge_refutes[1] = g_judge_refutes[2] = 0;
      assert(wfe_implement_adversarial_ok(".") == 1);
      setenv("AIMEE_AUTONOMY_SKEPTICS", "3", 1);
      /* judge cannot run -> fail closed */
      g_judge_rc = -1;
      g_judge_call = 0;
      g_judge_refutes[0] = 0;
      assert(wfe_implement_adversarial_ok(".") == 0);

      wfe_set_judge_provider(NULL);
      unsetenv("AIMEE_AUTONOMY_SKEPTICS");
   }

   /* E: per-work-item git worktree (F2) — ensure creates + persists + is
    *    idempotent; cleanup removes it. */
   {
      char repo[] = "/tmp/wfe_f2_repo_XXXXXX";
      assert(wfe_test_mkdtemp(repo));
      char cmd[640];
      snprintf(cmd, sizeof cmd,
               "cd %s && git init -q && git -c user.email=t@t -c user.name=t "
               "commit -q --allow-empty -m base",
               repo);
      assert(system(cmd) == 0);

      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("ds", "f2repo", "f2prop", "autonomous", id, err, sizeof err) ==
             0);

      char wt[1024] = "";
      assert(wfe_worktree_ensure(id, "", repo, "HEAD", wt, sizeof wt) == 0);
      assert(wt[0]);
      struct stat stt;
      assert(stat(wt, &stt) == 0); /* the worktree exists */

      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.worktree, wt) == 0); /* persisted on the work item */

      char wt2[1024] = ""; /* idempotent: an existing worktree is returned as-is */
      assert(wfe_worktree_ensure(id, wt, repo, "HEAD", wt2, sizeof wt2) == 0);
      assert(strcmp(wt2, wt) == 0);

      assert(wfe_worktree_cleanup(wt, repo) == 0);
      assert(stat(wt, &stt) != 0); /* removed */

      snprintf(cmd, sizeof cmd, "rm -rf %s", repo);
      (void)system(cmd);
   }

   /* F: orphan-worktree GC + inode cap — reap worktrees no LIVE item owns, keep
    *    active ones, honour the grace window, and fail-closed at the cap. */
   {
      char repo[] = "/tmp/wfe_gc_repo_XXXXXX";
      assert(wfe_test_mkdtemp(repo));
      char cmd[640];
      snprintf(cmd, sizeof cmd,
               "cd %s && git init -q && git -c user.email=t@t -c user.name=t "
               "commit -q --allow-empty -m base",
               repo);
      assert(system(cmd) == 0);
      struct stat stt;
      char err[256] = "";
      /* Start from an empty worktree pool so counts are deterministic (earlier
       * blocks left terminal-item worktrees under the shared AIMEE_HOME). */
      snprintf(cmd, sizeof cmd, "rm -rf %s/wfe-worktrees", home);
      (void)system(cmd);

      /* (1) an active item's worktree survives a grace=0 GC (it still owns it). */
      char live_id[80] = "";
      assert(wfe_work_item_create("ds", "gcL", "gcpL", "autonomous", live_id, err, sizeof err) ==
             0);
      char wl[1024] = "";
      assert(wfe_worktree_ensure(live_id, "", repo, "HEAD", wl, sizeof wl) == 0);
      assert(wfe_worktree_orphan_gc(repo, 0) == 0); /* nothing reapable */
      assert(stat(wl, &stt) == 0);                  /* kept */

      /* (2) a TERMINAL item's stranded worktree is reaped. */
      char term_id[80] = "";
      assert(wfe_work_item_create("ds", "gcT", "gcpT", "autonomous", term_id, err, sizeof err) ==
             0);
      char wtm[1024] = "";
      assert(wfe_worktree_ensure(term_id, "", repo, "HEAD", wtm, sizeof wtm) == 0);
      assert(db1_work_item_set_terminal(term_id, "abandoned") == 0);
      assert(wfe_worktree_orphan_gc(repo, 0) == 1);
      assert(stat(wtm, &stt) != 0); /* reaped */
      assert(stat(wl, &stt) == 0);  /* the active one is untouched */

      /* (3) a VANISHED row reaps its worktree, but only past the grace window. */
      char orph_id[80] = "";
      assert(wfe_work_item_create("ds", "gcO", "gcpO", "autonomous", orph_id, err, sizeof err) ==
             0);
      char wo[1024] = "";
      assert(wfe_worktree_ensure(orph_id, "", repo, "HEAD", wo, sizeof wo) == 0);
      assert(db1_work_item_delete(orph_id) == 0);
      assert(wfe_worktree_orphan_gc(repo, 100000) == 0); /* too fresh -> kept */
      assert(stat(wo, &stt) == 0);
      assert(wfe_worktree_orphan_gc(repo, 0) == 1); /* grace 0 -> reaped */
      assert(stat(wo, &stt) != 0);

      /* (4) inode cap fails closed: with MAX=1 and the active worktree holding the
       *     one slot, a second ensure can't allocate and returns -1 (the caller
       *     then degrades to the shared checkout). */
      setenv("AIMEE_WFE_WORKTREE_MAX", "1", 1);
      char cap_id[80] = "";
      assert(wfe_work_item_create("ds", "gcC", "gcpC", "autonomous", cap_id, err, sizeof err) == 0);
      char wc[1024] = "";
      assert(wfe_worktree_ensure(cap_id, "", repo, "HEAD", wc, sizeof wc) == -1);
      unsetenv("AIMEE_WFE_WORKTREE_MAX");

      snprintf(cmd, sizeof cmd, "rm -rf %s", repo);
      (void)system(cmd);
   }

   /* F: wfe_base_is_feature — the merge-seam hard gate's predicate. ONLY an
    *    aimee-managed feature branch is an autonomous merge target; every
    *    integration/protected/other base (and an empty/unknown base) is refused,
    *    so a misconfigured pr.open base cannot merge work into a non-feature branch. */
   assert(wfe_base_is_feature("aimee/feat/wi_x") == 1);
   assert(wfe_base_is_feature("aimee/feat/") == 1); /* prefix match is sufficient */
   assert(wfe_base_is_feature("testing") == 0);
   assert(wfe_base_is_feature("main") == 0);
   assert(wfe_base_is_feature("aimee/wi/x.s0") == 0); /* aimee-namespaced but not feat/ */
   assert(wfe_base_is_feature("xaimee/feat/y") == 0); /* prefix must anchor at start */
   assert(wfe_base_is_feature("") == 0);
   assert(wfe_base_is_feature(NULL) == 0);

   printf("ok\n");
   return 0;
}
