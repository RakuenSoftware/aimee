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
static wfe_ci_status_t m_ci(const char *r, const char *p)
{
   (void)r;
   (void)p;
   return g_ci;
}
static int m_mergeable(const char *r, const char *p)
{
   (void)r;
   (void)p;
   return g_mergeable;
}
static int m_is_merged(const char *r, const char *p)
{
   (void)r;
   (void)p;
   return g_is_merged;
}
static wfe_merge_result_t m_merge(const char *r, const char *p)
{
   (void)r;
   (void)p;
   return g_merge;
}
/* open is unused by the safety blocks (ci/mergeable/merge); NULL it explicitly so
 * the positional initializer covers every wfe_forge_t field (-Werror=missing-
 * field-initializers). */
static const wfe_forge_t MOCK = {m_ci, m_mergeable, m_is_merged, m_merge, NULL};

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

static int run_fresh(const char *path_suffix)
{
   char id[80] = "", err[256] = "";
   if (wfe_work_item_create("sf", "r", path_suffix, "interactive", id, err, sizeof err) != 0)
   {
      return -99;
   }
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

   printf("ok\n");
   return 0;
}
