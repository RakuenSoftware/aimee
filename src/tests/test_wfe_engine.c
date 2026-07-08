/* test_wfe_engine.c -- W2: work-item state machine + engine. Exercises
 * create -> run -> accepted, the audit log, loop-back max_attempts pause, and
 * the cost-cap pause, using deterministic executors. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_store.h"
#include "wfe_engine.h"

/* mini workflow: draft -> gate -> pr -> done(merge); gate loops to draft. */
static const char *MINI = "name: mini\n"
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

/* Loop-cap variants of `mini`: the gate carries max_iters:2 + on_max, so with an
 * always-loop gate executor the cap is hit after 2 loops and resolves per on_max. */
#define MINI_CAP_BODY(policy)                                                                      \
   "start: draft\nnodes:\n"                                                                        \
   "  - id: draft\n    block: author.proposal\n    next: gate\n"                                   \
   "  - id: gate\n    block: gate.roundtable\n    in:\n      src: draft.out\n    params:\n"        \
   "      panel:\n        required:\n          - security\n          - architect\n"               \
   "      max_iters: 2\n      on_max: " policy "\n    on_pass: pr\n    on_fail: draft\n"           \
   "  - id: pr\n    block: pr.open\n    in:\n      src: draft.out\n    next: done\n"               \
   "  - id: done\n    block: merge\n    in:\n      pr: pr.out\n"
static const char *MINI_FAIL = "name: minifail\n" MINI_CAP_BODY("fail");
static const char *MINI_PASS = "name: minipass\n" MINI_CAP_BODY("pass");
static const char *MINI_HUMAN = "name: minihuman\n" MINI_CAP_BODY("human");

/* always-loop executor (forces on_fail) */
static wfe_step_result_t exec_loop(wfe_ctx *c, const wfe_node_t *n)
{
   (void)c;
   (void)n;
   return wfe_step_looped();
}
/* costed advance executor */
static wfe_step_result_t exec_cost(wfe_ctx *c, const wfe_node_t *n)
{
   (void)c;
   char h[80];
   snprintf(h, sizeof h, "%s.out", n->id);
   return wfe_step_advanced(h, "deadbeef", 1.0);
}

static void setup_home(void)
{
   char tmpl[] = "/tmp/wfe_home_XXXXXX";
   char *dir = mkdtemp(tmpl);
   assert(dir);
   char wf[512];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   char path[600];
   snprintf(path, sizeof path, "%s/mini.yaml", wf);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(MINI, f);
   fclose(f);
   setenv("AIMEE_HOME", dir, 1);
}

/* Write an extra workflow YAML into $AIMEE_HOME/workflows (set by setup_home). */
static void write_workflow(const char *name, const char *yaml)
{
   const char *home = getenv("AIMEE_HOME");
   assert(home);
   char path[700];
   snprintf(path, sizeof path, "%s/workflows/%s.yaml", home, name);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(yaml, f);
   fclose(f);
}

int main(void)
{
   printf("wfe-engine: ");
   setup_home();
   assert(db1_init(":memory:") == 0);

   /* --- A: create -> run -> accepted, with audit trail --- */
   {
      wfe_reset_block_executors();
      wfe_register_stub_executors();
      char id[80] = "";
      char err[256] = "";
      int rc = wfe_work_item_create("mini", "git@github.com:x/y.git", "docs/p.md", "interactive",
                                    id, err, sizeof err);
      if (rc != 0)
         printf("\n  create failed: %s\n", err);
      assert(rc == 0);
      assert(strncmp(id, "wi_", 3) == 0);

      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.current_stage, "draft") == 0);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.repo, "git@github.com:x/y") == 0); /* .git stripped */

      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "accepted") == 0);

      db1_lifecycle_event_t *evs = NULL;
      int ne = db1_lifecycle_event_list(id, &evs);
      assert(ne >= 2);
      int saw_terminal = 0;
      for (int i = 0; i < ne; i++)
         if (strcmp(evs[i].kind, "terminal") == 0)
            saw_terminal = 1;
      assert(saw_terminal);
      free(evs);
   }

   /* --- B: loop-back hits max_attempts -> pending_human pause --- */
   {
      wfe_reset_block_executors();
      wfe_register_stub_executors();
      wfe_register_block_executor(WFE_BLK_GATE_ROUNDTABLE, exec_loop);
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("mini", "r2", "p2.md", "interactive", id, err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0);
      assert(strcmp(wi.pause_reason, "pending_human") == 0);
   }

   /* --- C: cost cap -> budget_exceeded pause --- */
   {
      wfe_reset_block_executors();
      wfe_register_stub_executors();
      wfe_register_block_executor(WFE_BLK_AUTHOR_PROPOSAL, exec_cost);
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("mini", "r3", "p3.md", "interactive", id, err, sizeof err) == 0);
      assert(db1_work_item_set_cost_cap(id, 0.5) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(wi.cum_cost_usd >= 1.0);
      assert(strcmp(wi.pause_reason, "budget_exceeded") == 0);
   }

   /* --- D: duplicate (repo, proposal_path) rejected --- */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("mini", "dup", "dup.md", "interactive", id, err, sizeof err) ==
             0);
      char id2[80] = "";
      assert(wfe_work_item_create("mini", "dup", "dup.md", "interactive", id2, err, sizeof err) !=
             0);
   }

   /* --- E: loop cap on_max=fail -> terminal "rejected" after max_iters loops --- */
   {
      write_workflow("minifail", MINI_FAIL);
      wfe_reset_block_executors();
      wfe_register_stub_executors();
      wfe_register_block_executor(WFE_BLK_GATE_ROUNDTABLE, exec_loop);
      char id[80] = "";
      char err[256] = "";
      assert(wfe_work_item_create("minifail", "r_fail", "pf.md", "interactive", id, err,
                                  sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "rejected") == 0); /* on_max:fail -> terminal reject */
   }

   /* --- F: loop cap on_max=pass -> forced forward via on_pass -> accepted --- */
   {
      write_workflow("minipass", MINI_PASS);
      wfe_reset_block_executors();
      wfe_register_stub_executors();
      wfe_register_block_executor(WFE_BLK_GATE_ROUNDTABLE, exec_loop);
      char id[80] = "";
      char err[256] = "";
      assert(wfe_work_item_create("minipass", "r_pass", "pp.md", "interactive", id, err,
                                  sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "accepted") == 0); /* forced-pass rode on_pass to completion */
   }

   /* --- G: loop cap on_max=human (+ explicit max_iters:2) -> pause pending_human --- */
   {
      write_workflow("minihuman", MINI_HUMAN);
      wfe_reset_block_executors();
      wfe_register_stub_executors();
      wfe_register_block_executor(WFE_BLK_GATE_ROUNDTABLE, exec_loop);
      char id[80] = "";
      char err[256] = "";
      assert(wfe_work_item_create("minihuman", "r_hum", "ph.md", "interactive", id, err,
                                  sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "active") == 0); /* paused, not terminal */
      assert(strcmp(wi.pause_reason, "pending_human") == 0);
   }

   printf("ok\n");
   return 0;
}
