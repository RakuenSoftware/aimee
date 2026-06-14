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
      assert(wfe_gate_override(id, "approve", "ship it", err, sizeof err) == 0);
      assert(wfe_engine_run(id, err, sizeof err) == 0);
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "accepted") == 0);
   }

   /* A5: override cap -> forced rejected on the (max+1)th */
   {
      char id[80] = "", err[256] = "";
      assert(wfe_work_item_create("auto", "a5", "a5", "interactive", id, err, sizeof err) == 0);
      int forced = 0;
      for (int k = 0; k < WFE_MAX_OVERRIDES + 1; k++)
      {
         int rc = wfe_gate_override(id, "approve", "x", err, sizeof err);
         if (rc == 1)
            forced = 1;
      }
      assert(forced);
      db1_work_item_t wi;
      assert(db1_work_item_get(id, &wi) == 1);
      assert(strcmp(wi.state, "rejected") == 0);
   }

   printf("ok\n");
   return 0;
}
