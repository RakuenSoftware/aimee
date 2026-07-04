/* test_wfe_autonomy.c -- W6: the autonomy driver + human-only gate-override. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_store.h"
#include "wfe_approval.h"
#include "wfe_autonomy.h"
#include "wfe_blocks.h"
#include "wfe_engine.h"
#include "wfe_iface.h"
#include "wfe_roundtable.h"

/* human gate with policy: preauthorized */
static const char *AUTO = "name: auto\n"
                          "start: draft\n"
                          "nodes:\n"
                          "  - id: draft\n"
                          "    block: author.proposal\n"
                          "    next: approve\n"
                          "  - id: approve\n"
                          "    block: gate.human\n"
                          "    in:\n"
                          "      src: draft.out\n"
                          "    params:\n"
                          "      policy: preauthorized\n"
                          "    next: pr\n"
                          "  - id: pr\n"
                          "    block: pr.open\n"
                          "    in:\n"
                          "      src: draft.out\n"
                          "    next: done\n"
                          "  - id: done\n"
                          "    block: merge\n"
                          "    in:\n"
                          "      pr: pr.out\n";

/* roundtable gate (left to the live §0 provider -> degraded) */
static const char *RT = "name: rta\n"
                        "start: draft\n"
                        "nodes:\n"
                        "  - id: draft\n"
                        "    block: author.proposal\n"
                        "    next: gate\n"
                        "  - id: gate\n"
                        "    block: gate.roundtable\n"
                        "    in:\n"
                        "      src: draft.out\n"
                        "    params:\n"
                        "      panel:\n"
                        "        required:\n"
                        "          - security\n"
                        "          - architect\n"
                        "    on_pass: pr\n"
                        "    on_fail: draft\n"
                        "  - id: pr\n"
                        "    block: pr.open\n"
                        "    in:\n"
                        "      src: draft.out\n"
                        "    next: done\n"
                        "  - id: done\n"
                        "    block: merge\n"
                        "    in:\n"
                        "      pr: pr.out\n";

/* This stub-executor test does not link wfe_blocks.o (it uses register_stub); the
 * autonomy driver's terminal cleanup calls wfe_worktree_cleanup, so provide a no-op
 * (the real helper is exercised in test_wfe_delegate_seam over a git fixture). */
int wfe_worktree_cleanup(const char *worktree, const char *repo_local)
{
   (void)worktree;
   (void)repo_local;
   return 0;
}

static void write_wf(const char *dir, const char *name, const char *body)
{
   char p[256];
   snprintf(p, sizeof p, "%s/workflows/%s.yaml", dir, name);
   FILE *f = fopen(p, "wb");
   assert(f);
   fputs(body, f);
   fclose(f);
}

int main(void)
{
   printf("wfe-autonomy: ");
   char d[] = "/tmp/wfe_auto_XXXXXX";
   char *dir = mkdtemp(d);
   assert(dir);
   char wf[128];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   write_wf(dir, "auto", AUTO);
   write_wf(dir, "rta", RT);
   setenv("AIMEE_HOME", dir, 1);
   assert(db1_init(":memory:") == 0);
   assert(wfe_approval_ensure_key() == 0);

   wfe_reset_block_executors();
   wfe_register_stub_executors();
   wfe_register_human_gate();
   wfe_register_roundtable_gate();
   wfe_set_panel_provider(NULL); /* live §0 -> degraded */

   /* A1: autonomous + preauthorized human gate -> auto-advances to accepted */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "a1", "a1", "autonomous", id, err, sizeof err) == 0);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "accepted") == 0);
   }

   /* A2: interactive + same gate -> parks pending_human (no auto-approval) */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "a2", "a2", "interactive", id, err, sizeof err) == 0);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.pause_reason, "pending_human") == 0);
   }

   /* A3: autonomous + roundtable (live §0) -> parks; never self-approves */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("rta", "a3", "a3", "autonomous", id, err, sizeof err) == 0);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strncmp(wi.pause_reason, "panel_", 6) == 0);
   }

   /* A4: gate-override resumes a parked item; cap forces rejected */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "a4", "a4", "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0); /* parks at human gate */
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "pending_human") == 0);
      assert(wfe_gate_override(id, "approve", "alice", "ship it", err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "accepted") == 0);
   }

   /* A5: override cap -> forced rejected on the (max+1)th */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "a5", "a5", "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0); /* park at the human gate "approve" */
      int forced = 0;
      for (int k = 0; k < WFE_MAX_OVERRIDES + 1; k++)
      {
         /* each override clears the pause; re-park (as if the gate re-fired after
          * a stale approval) so the next override is on a parked item. */
         db1_work_item_set_pause(id, "pending_human", "approve");
         int rc = wfe_gate_override(id, "approve", "alice", "x", err, sizeof err);
         if (rc == 1)
            forced = 1;
      }
      assert(forced);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "rejected") == 0);
   }

   /* A6: autonomous merge-target rail (WP-5) — protected branches are refused;
    * the configured base must be non-protected for pr.open/merge to proceed. */
   {
      assert(wfe_base_is_protected("main"));
      assert(wfe_base_is_protected("master"));
      assert(wfe_base_is_protected("Main"));   /* case-insensitive */
      assert(wfe_base_is_protected("MASTER")); /* case-insensitive */
      assert(wfe_base_is_protected("release-1.2"));
      assert(wfe_base_is_protected("release/v2.0"));
      assert(wfe_base_is_protected("")); /* empty -> protected (fail closed) */
      assert(!wfe_base_is_protected("testing"));
      assert(!wfe_base_is_protected("aimee/wi/abc"));
      assert(!wfe_base_is_protected("release-notes-edit")); /* not the release train */
      unsetenv("AIMEE_AUTONOMY_BASE");
      assert(strcmp(wfe_autonomous_base(), "testing") == 0);
      assert(wfe_autonomous_target_ok());
      setenv("AIMEE_AUTONOMY_BASE", "main", 1); /* misconfig -> guard refuses */
      assert(!wfe_autonomous_target_ok());
      setenv("AIMEE_AUTONOMY_BASE", "dev", 1);
      assert(wfe_autonomous_target_ok());
      unsetenv("AIMEE_AUTONOMY_BASE");
   }

   /* A7: per-run turn cap (WP-5) — once the cumulative audit-event count reaches
    * the cap, an autonomous run parks budget_exceeded BEFORE advancing further
    * (so a runaway loop can't burn unbounded turns). */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "cap1", "cap1", "autonomous", id, err, sizeof err) == 0);
      for (int k = 0; k < 5; k++)
         db1_lifecycle_event_add(id, "draft", "test", "t", "pad", "", 0);
      setenv("AIMEE_AUTONOMY_MAX_TURNS", "3", 1);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      unsetenv("AIMEE_AUTONOMY_MAX_TURNS");
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.pause_reason, "budget_exceeded") == 0);
   }

   /* A8: server-side cost estimate (WP-5) — wall-clock seconds * rate; negative
    * elapsed clamps to 0; the rate is env-overridable. */
   {
      unsetenv("AIMEE_AUTONOMY_USD_PER_SEC");
      assert(wfe_autonomy_cost_estimate(0.0) == 0.0);
      assert(wfe_autonomy_cost_estimate(-5.0) == 0.0); /* clamp */
      double c = wfe_autonomy_cost_estimate(10.0);     /* 10s * 0.0005 = 0.005 */
      assert(c > 0.0049 && c < 0.0051);
      setenv("AIMEE_AUTONOMY_USD_PER_SEC", "0.01", 1);
      c = wfe_autonomy_cost_estimate(10.0); /* 10s * 0.01 = 0.1 */
      assert(c > 0.099 && c < 0.101);
      setenv("AIMEE_AUTONOMY_USD_PER_SEC", "junk", 1); /* malformed -> default */
      c = wfe_autonomy_cost_estimate(10.0);
      assert(c > 0.0049 && c < 0.0051);
      unsetenv("AIMEE_AUTONOMY_USD_PER_SEC");
   }

   /* A9: an UNRECOVERABLE advance failure (current_stage is not a node in the
    * workflow — e.g. an orphaned stage after a rename, or the same class as a
    * looped node with no on_fail edge) parks the item 'stuck' instead of the
    * scheduler retry-failing it forever while the UI shows "running". A re-sweep
    * is a no-op: still stuck, no new lifecycle event (no spam). */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "stuck1", "stuck1", "autonomous", id, err, sizeof err) ==
             0);
      /* orphan the stage: "ghoststage" is not a node in the "auto" workflow. */
      assert(db1_work_item_set_stage(id, "ghoststage", "") == 0);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.pause_reason, "stuck") == 0);

      db1_lifecycle_event_t *evs = NULL;
      int before = db1_lifecycle_event_list(id, &evs);
      free(evs);
      assert(wfe_autonomy_run(id, err, sizeof err) == 0); /* re-sweep: early-out */
      evs = NULL;
      int after = db1_lifecycle_event_list(id, &evs);
      free(evs);
      assert(after == before); /* no re-park spam */
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.pause_reason, "stuck") == 0);
   }

   /* PC2: the CI-event webhook routes by pr_ref -> work-item id. */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "pc2repo", "pc2prop", "autonomous", id, err,
                                  sizeof err) == 0);
      assert(db1_work_item_set_pr_ref(id, "https://github.com/o/r/pull/77") == 0);
      char got[80] = "";
      assert(db1_work_item_id_by_pr_ref("https://github.com/o/r/pull/77", got, sizeof got) == 1);
      assert(strcmp(got, id) == 0);
      /* an unknown pr_ref resolves to none (webhook returns 404) */
      char none[80] = "x";
      assert(db1_work_item_id_by_pr_ref("https://github.com/o/r/pull/999", none, sizeof none) == 0);
      assert(none[0] == '\0');
      /* empty / NULL pr_ref is a bad arg */
      assert(db1_work_item_id_by_pr_ref("", got, sizeof got) == -1);
      assert(db1_work_item_id_by_pr_ref(NULL, got, sizeof got) == -1);
   }

   printf("ok\n");
   return 0;
}
