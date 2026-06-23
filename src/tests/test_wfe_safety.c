/* test_wfe_safety.c -- gate.ci / check.mergeable / idempotent merge via a mock
 * forge provider, exercised through the engine. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_store.h"
#include "wfe_blocks.h"
#include "wfe_def.h"
#include "wfe_engine.h"
#include "wfe_iface.h"

/* configurable mock forge */
static wfe_ci_status_t g_ci;
static int g_mergeable, g_is_merged;
static wfe_merge_result_t g_merge;
/* the pr_ref the downstream gates were called with (last seen), so a test can
 * assert pr.open's ref reaches gate.ci / check.mergeable / merge. */
static char g_seen_pr[160];
static void see_pr(const char *p)
{
   snprintf(g_seen_pr, sizeof g_seen_pr, "%s", p ? p : "");
}
static wfe_ci_status_t m_ci(const char *r, const char *p)
{
   (void)r;
   see_pr(p);
   return g_ci;
}
static int m_mergeable(const char *r, const char *p)
{
   (void)r;
   see_pr(p);
   return g_mergeable;
}
static int m_is_merged(const char *r, const char *p)
{
   (void)r;
   see_pr(p);
   return g_is_merged;
}
static wfe_merge_result_t m_merge(const char *r, const char *p)
{
   (void)r;
   see_pr(p);
   return g_merge;
}
/* a PR-opening provider: writes a known forge ref so we can assert it propagates. */
static int m_open(const char *repo, const char *branch, const char *title, const char *body,
                  char out_pr_ref[128])
{
   (void)repo;
   (void)branch;
   (void)title;
   (void)body;
   snprintf(out_pr_ref, 128, "PR#42");
   return 0;
}
/* open that signals success but writes a junk (empty) ref -> must be rejected. */
static int m_open_bad(const char *repo, const char *branch, const char *title, const char *body,
                      char out_pr_ref[128])
{
   (void)repo;
   (void)branch;
   (void)title;
   (void)body;
   out_pr_ref[0] = '\0';
   return 0;
}
/* open that fails outright. */
static int m_open_fail(const char *repo, const char *branch, const char *title, const char *body,
                       char out_pr_ref[128])
{
   (void)repo;
   (void)branch;
   (void)title;
   (void)body;
   (void)out_pr_ref;
   return -1;
}
/* MOCK: no `open` (today's default) -> pr_ref falls back to the work-item id.
 * MOCK_OPEN: opens "PR#42" -> the gates must resolve that, not the work-item id.
 * MOCK_OPENBAD / MOCK_OPENFAIL: pr.open must fail closed, never reaching the gates. */
static const wfe_forge_t MOCK = {m_ci, m_mergeable, m_is_merged, m_merge, NULL};
static const wfe_forge_t MOCK_OPEN = {m_ci, m_mergeable, m_is_merged, m_merge, m_open};
static const wfe_forge_t MOCK_OPENBAD = {m_ci, m_mergeable, m_is_merged, m_merge, m_open_bad};
static const wfe_forge_t MOCK_OPENFAIL = {m_ci, m_mergeable, m_is_merged, m_merge, m_open_fail};

/* pp -> pr -> check.mergeable -> gate.ci -> merge; gates loop back to pp. */
static const char *WF = "name: sf\nstart: pp\nnodes:\n"
                        "  - id: pp\n    block: author.proposal\n    next: pr\n"
                        "  - id: pr\n    block: pr.open\n    in:\n      src: pp.out\n"
                        "    next: cm\n"
                        "  - id: cm\n    block: check.mergeable\n    in:\n      pr: pr.out\n"
                        "    on_pass: ci\n    on_fail: pp\n"
                        "  - id: ci\n    block: gate.ci\n    in:\n      pr: pr.out\n"
                        "    on_pass: m\n    on_fail: pp\n"
                        "  - id: m\n    block: merge\n    in:\n      pr: pr.out\n";

static char g_last_id[80]; /* the work-item id of the most recent run_fresh */
static int run_fresh(const char *path_suffix)
{
   char id[80] = "", err[256] = "";
   if (wfe_work_item_create("sf", "r", path_suffix, "interactive", id, err, sizeof err) != 0)
   {
      return -99;
   }
   snprintf(g_last_id, sizeof g_last_id, "%s", id);
   if (wfe_engine_run(id, err, sizeof err) != 0)
   {
      return -98;
   }
   db1_work_item_t wi;
   if (db1_work_item_get(id, &wi) != 1)
      return -97;
   if (strcmp(wi.state, "accepted") == 0)
      return 1; /* merged */
   if (strcmp(wi.pause_reason, "ci_pending") == 0)
      return 2; /* parked on CI */
   if (strcmp(wi.pause_reason, "pending_human") == 0)
      return 3; /* looped out */
   if (strcmp(wi.pause_reason, "merge_pending") == 0)
      return 4; /* merge state undeterminable -> parked */
   return 0;
}

int main(void)
{
   printf("wfe-safety: ");
   char home[] = "/tmp/wfe_sft_XXXXXX";
   assert(mkdtemp(home));
   char wf[128];
   snprintf(wf, sizeof wf, "%s/workflows", home);
   mkdir(wf, 0755);
   char p[256];
   snprintf(p, sizeof p, "%s/workflows/sf.yaml", home);
   FILE *f = fopen(p, "wb");
   assert(f);
   fputs(WF, f);
   fclose(f);
   setenv("AIMEE_HOME", home, 1);
   assert(db1_init(":memory:") == 0);

   wfe_reset_block_executors();
   wfe_register_default_executors();
   wfe_set_forge_provider(&MOCK);

   /* A: all green + not-yet-merged -> merge OK -> accepted */
   g_mergeable = 1;
   g_ci = WFE_CI_SUCCESS;
   g_is_merged = 0;
   g_merge = WFE_MERGE_OK;
   assert(run_fresh("a") == 1);

   /* B: CI failing -> gate.ci loops -> max_attempts -> pending_human (not merged) */
   g_ci = WFE_CI_FAILURE;
   assert(run_fresh("b") == 3);

   /* C: CI still running -> park ci_pending (never advances/merges) */
   g_ci = WFE_CI_PENDING;
   assert(run_fresh("c") == 2);

   /* D: merge conflict -> check.mergeable loops -> pending_human */
   g_ci = WFE_CI_SUCCESS;
   g_mergeable = 0;
   assert(run_fresh("d") == 3);

   /* E: already merged -> idempotent no-op success (merge fn never reached) */
   g_mergeable = 1;
   g_ci = WFE_CI_SUCCESS;
   g_is_merged = 1;
   g_merge = WFE_MERGE_ERROR; /* would fail if called; is_merged short-circuits */
   assert(run_fresh("e") == 1);

   /* F: mergeability undeterminable (forge error) -> check.mergeable parks
    *    (merge_pending), never advances on an unknown state. */
   g_mergeable = -1;
   g_ci = WFE_CI_SUCCESS;
   g_is_merged = 0;
   g_merge = WFE_MERGE_OK;
   assert(run_fresh("f") == 4);

   /* G: merge-state undeterminable at the merge step -> park (merge_pending);
    *    merge() is never called, so a transient error cannot double-merge. */
   g_mergeable = 1;
   g_ci = WFE_CI_SUCCESS;
   g_is_merged = -1;       /* cannot confirm not-already-merged */
   g_merge = WFE_MERGE_OK; /* would merge if (wrongly) called */
   assert(run_fresh("g") == 4);

   /* H: fallback (no `open` provider, today's MOCK) -> the gates still receive a
    *    non-empty ref, namely the work-item id. Locks the no-regression path. */
   g_mergeable = 1;
   g_ci = WFE_CI_SUCCESS;
   g_is_merged = 0;
   g_merge = WFE_MERGE_OK;
   g_seen_pr[0] = '\0';
   assert(run_fresh("h") == 1);
   assert(strcmp(g_seen_pr, g_last_id) == 0); /* fell back to the work-item id */

   /* I: with a PR-opening provider, pr.open records "PR#42" and the downstream
    *    gates must resolve THAT ref, not the work-item id. This is the bug fix:
    *    before it, g_seen_pr would be the work-item id. */
   wfe_set_forge_provider(&MOCK_OPEN);
   g_mergeable = 1;
   g_ci = WFE_CI_SUCCESS;
   g_is_merged = 0;
   g_merge = WFE_MERGE_OK;
   g_seen_pr[0] = '\0';
   assert(run_fresh("i") == 1);
   assert(strcmp(g_seen_pr, "PR#42") == 0);   /* the real PR ref propagated */
   assert(strcmp(g_seen_pr, g_last_id) != 0); /* and it is NOT the work-item id */

   /* J: open succeeds but yields a junk (empty) ref -> pr.open fails closed; the
    *    merge gates are never reached, so no wrong-ref query and no merge. */
   wfe_set_forge_provider(&MOCK_OPENBAD);
   g_seen_pr[0] = '\0';
   assert(run_fresh("j") != 1);  /* not merged */
   assert(g_seen_pr[0] == '\0'); /* gates never ran */

   /* K: open fails outright -> same fail-closed guarantee. */
   wfe_set_forge_provider(&MOCK_OPENFAIL);
   g_seen_pr[0] = '\0';
   assert(run_fresh("k") != 1);
   assert(g_seen_pr[0] == '\0');

   printf("ok\n");
   return 0;
}
