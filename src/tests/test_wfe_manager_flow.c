/* test_wfe_manager_flow.c -- integration: drive the primary-as-manager executors
 * (understand -> split -> review -> gate.deliver) through the REAL workflow
 * engine, with a mock delegate provider that writes schema-valid artifacts and
 * stub-advance executors for implement/freeze/roundtable (so the test needs no
 * git worktree or live panel). Asserts the enforced workflow reaches delivery
 * (crosses gate.deliver -> accepted) and the lifecycle trail records each
 * manager step's advance. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_blocks.h"
#include "wfe_engine.h"
#include "wfe_iface.h"
#include "wfe_store.h"

/* managed-change shape: understand -> split -> implement -> freeze -> review ->
 * gate.roundtable -> gate.deliver. Distinct node ids so the mock can pick the
 * right schema from the per-node artifact path. */
static const char *WF = "name: mc\n"
                        "enforced: true\n"
                        "start: understand\n"
                        "nodes:\n"
                        "  - id: understand\n"
                        "    block: understand\n"
                        "    next: split\n"
                        "  - id: split\n"
                        "    block: split\n"
                        "    in:\n"
                        "      intent: understand.out\n"
                        "    next: implement\n"
                        "  - id: implement\n"
                        "    block: implement\n"
                        "    in:\n"
                        "      plan: split.out\n"
                        "    next: freeze\n"
                        "  - id: freeze\n"
                        "    block: freeze\n"
                        "    in:\n"
                        "      branch: implement.out\n"
                        "    next: review\n"
                        "  - id: review\n"
                        "    block: review\n"
                        "    in:\n"
                        "      src: freeze.out\n"
                        "    params:\n"
                        "      reviewer: contrarian\n"
                        "    on_pass: rt\n"
                        "    on_fail: split\n"
                        "  - id: rt\n"
                        "    block: gate.roundtable\n"
                        "    in:\n"
                        "      src: freeze.out\n"
                        "    params:\n"
                        "      panel:\n"
                        "        required:\n"
                        "          - security\n"
                        "          - architect\n"
                        "    on_pass: deliver\n"
                        "    on_fail: split\n"
                        "  - id: deliver\n"
                        "    block: gate.deliver\n"
                        "    in:\n"
                        "      verdict: rt.out\n";

/* mock delegate provider: writes a schema-valid artifact keyed on the node in
 * the artifact path (understand -> intent, split -> packets, review -> verdict). */
static int mock_delegate(const char *wd, const char *role, const char *prompt,
                         const char *artifact_path, char out_sha[64], char *err, size_t errlen)
{
   (void)wd;
   (void)role;
   (void)prompt;
   (void)err;
   (void)errlen;
   snprintf(out_sha, 64, "%s", "cafe0001");
   if (!artifact_path || !artifact_path[0])
      return 0;
   const char *j = NULL;
   if (strstr(artifact_path, "understand"))
      j = "{\"schema_version\":1,\"status\":\"unconfirmed\",\"summary\":\"add x\","
          "\"acceptance_criteria\":[\"works\"]}";
   else if (strstr(artifact_path, "split"))
      j = "{\"schema_version\":1,\"packets\":[{\"packet_id\":\"p1\",\"summary\":\"do x\","
          "\"target_blocks\":[\"implement\"],\"dependencies\":[],\"acceptance_criteria\":[\"ok\"]}]"
          "}";
   else if (strstr(artifact_path, "review"))
      j = "{\"schema_version\":1,\"verdict\":\"pass\",\"blocking_findings\":[],\"non_blocking\":[]"
          "}";
   if (j)
   {
      FILE *f = fopen(artifact_path, "wb");
      if (f)
      {
         fwrite(j, 1, strlen(j), f);
         fclose(f);
      }
   }
   return 0;
}
static const wfe_delegate_provider_t MOCK = {mock_delegate};

/* ---- TDD implement (two agents): a recording mock that captures the role
 * (persona) of every dispatch + whether the prompt is the RED/GREEN step, and
 * still writes a valid intent so understand advances into implement. ---- */
#define REC_MAX 32
static char g_rec_role[REC_MAX][32];
static int g_rec_red[REC_MAX];   /* prompt carried the RED (test-author) step */
static int g_rec_green[REC_MAX]; /* prompt carried the GREEN (implementer) step */
static int g_rec_n;
static int rec_delegate(const char *wd, const char *role, const char *prompt,
                        const char *artifact_path, char out_sha[64], char *err, size_t errlen)
{
   (void)wd;
   (void)err;
   (void)errlen;
   snprintf(out_sha, 64, "cafe0003");
   if (g_rec_n < REC_MAX)
   {
      snprintf(g_rec_role[g_rec_n], 32, "%s", role ? role : "");
      g_rec_red[g_rec_n] = prompt && strstr(prompt, "RED") != NULL;
      g_rec_green[g_rec_n] = prompt && strstr(prompt, "GREEN") != NULL;
      g_rec_n++;
   }
   /* let understand advance so the run reaches implement (implement then parks at
    * the freeze against the non-git repo, AFTER its dispatches have fired). */
   if (artifact_path && artifact_path[0] && strstr(artifact_path, "understand"))
   {
      FILE *f = fopen(artifact_path, "wb");
      if (f)
      {
         const char *j = "{\"schema_version\":1,\"status\":\"unconfirmed\",\"summary\":\"x\","
                         "\"acceptance_criteria\":[\"ok\"]}";
         fwrite(j, 1, strlen(j), f);
         fclose(f);
      }
   }
   return 0;
}
static const wfe_delegate_provider_t REC = {rec_delegate};

/* understand -> implement, implement in TDD mode: a test author (test_delegate)
 * then the implementer (delegate). Non-enforced (terminates at implement). */
static const char *WF_TDD = "name: tddwf\n"
                            "start: understand\n"
                            "nodes:\n"
                            "  - id: understand\n"
                            "    block: understand\n"
                            "    next: impl\n"
                            "  - id: impl\n"
                            "    block: implement\n"
                            "    in:\n"
                            "      intent: understand.out\n"
                            "    params:\n"
                            "      tdd: true\n"
                            "      persona: builder\n"
                            "      test_persona: sdet\n";

/* the same shape with TDD OFF: a single implementer dispatch, no test author. */
static const char *WF_PLAIN = "name: plainwf\n"
                              "start: understand\n"
                              "nodes:\n"
                              "  - id: understand\n"
                              "    block: understand\n"
                              "    next: impl\n"
                              "  - id: impl\n"
                              "    block: implement\n"
                              "    in:\n"
                              "      intent: understand.out\n";

/* stub-advance: stand in for implement/freeze/roundtable so the test avoids git
 * and the live panel; the engine still logs an "advance" for the node (which
 * gate.deliver's re-verify relies on for the roundtable gate). */
static wfe_step_result_t stub_adv(wfe_ctx *c, const wfe_node_t *n)
{
   (void)c;
   char h[80];
   snprintf(h, sizeof h, "%s.out", n->id);
   return wfe_step_advanced(h, "beef0002", 0.0);
}

static void setup_home(void)
{
   char tmpl[] = "/tmp/wfe_mgr_home_XXXXXX";
   char *dir = wfe_test_mkdtemp(tmpl);
   assert(dir);
   char wf[512];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   char path[600];
   snprintf(path, sizeof path, "%s/mc.yaml", wf);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(WF, f);
   fclose(f);
   snprintf(path, sizeof path, "%s/tddwf.yaml", wf);
   f = fopen(path, "wb");
   assert(f);
   fputs(WF_TDD, f);
   fclose(f);
   snprintf(path, sizeof path, "%s/plainwf.yaml", wf);
   f = fopen(path, "wb");
   assert(f);
   fputs(WF_PLAIN, f);
   fclose(f);
   setenv("AIMEE_HOME", dir, 1);
   /* producing blocks act in this writable dir (the mock writes artifacts here). */
   char repo[600];
   snprintf(repo, sizeof repo, "%s/repo", dir);
   mkdir(repo, 0755);
   setenv("AIMEE_WORKFLOW_REPO", repo, 1);
}

int main(void)
{
   printf("wfe-manager-flow: ");
   setup_home();
   assert(db1_init(":memory:") == 0);

   wfe_reset_block_executors();
   wfe_register_default_executors(); /* real understand/split/review/gate.deliver */
   wfe_set_delegate_provider(&MOCK); /* feeds understand/split/review */
   wfe_register_block_executor(WFE_BLK_IMPLEMENT, stub_adv); /* avoid git */
   wfe_register_block_executor(WFE_BLK_FREEZE, stub_adv);    /* avoid git */
   wfe_register_block_executor(WFE_BLK_GATE_ROUNDTABLE,
                               stub_adv); /* avoid live panel; logs advance */

   char id[80] = "", err[256] = "";
   int rc = wfe_work_item_create("mc", "git@github.com:x/y.git", "docs/p.md", "interactive", id,
                                 err, sizeof err);
   if (rc != 0)
      printf("\n  create failed: %s\n", err);
   assert(rc == 0);

   assert(wfe_engine_run(id, err, sizeof err) == 0);

   db1_work_item_t wi;
   assert(db1_work_item_get(id, &wi) == 1);
   if (strcmp(wi.state, "accepted") != 0)
      printf("\n  state=%s stage=%s pause=%s\n", wi.state, wi.current_stage, wi.pause_reason);
   assert(strcmp(wi.state, "accepted") == 0); /* crossed gate.deliver */

   /* the manager steps + the delivery all advanced, in order, in the audit log */
   db1_lifecycle_event_t *ev = NULL;
   int ne = db1_lifecycle_event_list(id, &ev);
   int adv_understand = 0, adv_split = 0, adv_review = 0, adv_deliver_terminal = 0;
   for (int i = 0; i < ne; i++)
   {
      if (strcmp(ev[i].kind, "advance") == 0 && strcmp(ev[i].stage, "understand") == 0)
         adv_understand = 1;
      if (strcmp(ev[i].kind, "advance") == 0 && strcmp(ev[i].stage, "split") == 0)
         adv_split = 1;
      if (strcmp(ev[i].kind, "advance") == 0 && strcmp(ev[i].stage, "review") == 0)
         adv_review = 1;
      if (strcmp(ev[i].kind, "terminal") == 0 && strcmp(ev[i].stage, "deliver") == 0)
         adv_deliver_terminal = 1;
   }
   free(ev);
   assert(adv_understand && adv_split && adv_review && adv_deliver_terminal);

   /* ---- TDD implement: two agents, tests before code ---- */
   wfe_reset_block_executors();
   wfe_register_default_executors(); /* REAL implement (not the stub above) */
   wfe_set_delegate_provider(&REC);

   /* TDD on: implement dispatches the test author (qa@tester) THEN the
    * implementer (engineer@coder), RED before GREEN. The run then parks at the
    * freeze (non-git repo), but both dispatches have already fired. */
   g_rec_n = 0;
   {
      char id2[80] = "", e2[256] = "";
      int rc2 = wfe_work_item_create("tddwf", "git@github.com:x/tdd.git", "docs/tdd.md",
                                     "interactive", id2, e2, sizeof e2);
      if (rc2 != 0)
         fprintf(stderr, "\n  tddwf create failed: %s\n", e2);
      assert(rc2 == 0);
      (void)wfe_engine_run(id2, e2, sizeof e2);
      /* the persona is carried in the dispatch role slot: test author = the
       * specified `test_persona` (sdet); implementer = the node's `persona`
       * (builder). Agent selection is the delegate system's routing decision, so
       * no per-step delegate name is threaded through the seam. */
      int i_test = -1, i_code = -1;
      for (int i = 0; i < g_rec_n; i++)
      {
         if (strcmp(g_rec_role[i], "sdet") == 0)
            i_test = i;
         if (strcmp(g_rec_role[i], "builder") == 0)
            i_code = i;
      }
      assert(i_test >= 0);         /* the test author ran with its specified persona */
      assert(i_code >= 0);         /* the implementer ran with its specified persona */
      assert(i_test < i_code);     /* RED (tests) before GREEN (code) */
      assert(g_rec_red[i_test]);   /* the test author got the RED prompt */
      assert(g_rec_green[i_code]); /* the implementer got the GREEN prompt */
   }

   /* TDD off: a plain implement dispatches exactly ONE implementer (engineer) and
    * NO separate test author (qa). */
   g_rec_n = 0;
   {
      char id3[80] = "", e3[256] = "";
      assert(wfe_work_item_create("plainwf", "git@github.com:x/plain.git", "docs/plain.md",
                                  "interactive", id3, e3, sizeof e3) == 0);
      (void)wfe_engine_run(id3, e3, sizeof e3);
      int qa_calls = 0, eng_calls = 0;
      for (int i = 0; i < g_rec_n; i++)
      {
         if (strcmp(g_rec_role[i], "qa") == 0)
            qa_calls++;
         if (strcmp(g_rec_role[i], "engineer") == 0)
            eng_calls++;
      }
      assert(qa_calls == 0);  /* no TDD -> no separate test author */
      assert(eng_calls == 1); /* the single implement dispatch */
   }

   printf("ok\n");
   return 0;
}
