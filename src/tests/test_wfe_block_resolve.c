/* test_wfe_block_resolve.c -- S2 sub-slice 4: the per-block resolver + the
 * dispatch-time externalization guard, against a real in-memory DB1 + engine
 * (stub executors). Covers the pure decision table, the DB resolve, and the
 * composed guard (default-off, deny/warn/allow, delivered lifts, audit). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_binding.h"
#include "wfe_block_resolve.h"
#include "wfe_engine.h"
#include "wfe_store.h"

/* ENFORCED workflow (terminal gate.deliver per I2), valid manager shape:
 * understand(READONLY) -> split -> implement -> freeze -> review -> gate.roundtable
 * -> gate.deliver(verdict). Stub executors. */
static const char *WF_ENF = "name: t\n"
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

/* NON-enforced workflow: the externalization guard must NOT apply. */
static const char *WF_PLAIN = "name: u\n"
                              "start: a\n"
                              "nodes:\n"
                              "  - id: a\n"
                              "    block: understand\n";

static void write_wf(const char *dir, const char *name, const char *body)
{
   char path[700];
   snprintf(path, sizeof path, "%s/workflows/%s.yaml", dir, name);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(body, f);
   fclose(f);
}

static void setup_home(void)
{
   char tmpl[] = "/tmp/wfe_res_home_XXXXXX";
   char *dir = mkdtemp(tmpl);
   assert(dir);
   char wf[512];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   write_wf(dir, "t", WF_ENF);
   write_wf(dir, "u", WF_PLAIN);
   setenv("AIMEE_HOME", dir, 1);
   char repo[600];
   snprintf(repo, sizeof repo, "%s/repo", dir);
   mkdir(repo, 0755);
   setenv("AIMEE_WORKFLOW_REPO", repo, 1);
}

static void test_decide_pure(void)
{
   /* off / advisory never restrict; soft warns; hard denies -- only when policy
    * actually blocks the call. */
   assert(wfe_toolcall_decide(WFE_ENFORCE_OFF, 1) == WFE_TC_ALLOW);
   assert(wfe_toolcall_decide(WFE_ENFORCE_ADVISORY, 1) == WFE_TC_ALLOW);
   assert(wfe_toolcall_decide(WFE_ENFORCE_SOFT, 1) == WFE_TC_WARN);
   assert(wfe_toolcall_decide(WFE_ENFORCE_HARD, 1) == WFE_TC_DENY);
   assert(wfe_toolcall_decide(WFE_ENFORCE_HARD, 0) == WFE_TC_ALLOW);
   assert(wfe_toolcall_decide(WFE_ENFORCE_SOFT, 0) == WFE_TC_ALLOW);
}

static int audit_count(const char *wi)
{
   db1_lifecycle_event_t *ev = NULL;
   int ne = db1_lifecycle_event_list(wi, &ev);
   int n = 0;
   for (int i = 0; i < ne; i++)
      if (strcmp(ev[i].actor, "enforce-s2") == 0 && strcmp(ev[i].kind, "toolcall_guard") == 0)
         n++;
   free(ev);
   return n;
}

int main(void)
{
   printf("wfe-block-resolve: ");
   test_decide_pure();

   setup_home();
   assert(db1_init(":memory:") == 0);
   wfe_reset_block_executors();
   wfe_register_stub_executors();

   char id[80] = "", err[256] = "";
   assert(wfe_work_item_create("t", "git@github.com:x/y.git", "docs/p.md", "interactive", id, err,
                               sizeof err) == 0);
   const char *SID = "sess-A";

   /* unbound -> resolve returns 0 */
   wfe_block_ctx_t ctx;
   assert(wfe_block_resolve(SID, &ctx) == 0);
   assert(ctx.bound == 0);

   assert(db1_wfe_bind(SID, id, "advisory") == 0);

   /* bound at understand: enforced, READONLY, not delivered, advanceable, stage a */
   assert(wfe_block_resolve(SID, &ctx) == 1);
   assert(ctx.bound == 1);
   assert(ctx.enforced == 1);
   assert(ctx.surface == WFE_SURFACE_READONLY);
   assert(ctx.delivered == 0);
   assert(ctx.advanceable == 1);
   assert(strcmp(ctx.stage, "understand") == 0);

   /* --- the guard --- */
   unsetenv("AIMEE_WORKFLOW_ENFORCE_STAGE");
   assert(wfe_mcp_toolcall_action(SID, "pr.open", NULL, 0) == WFE_TC_ALLOW); /* dial off */

   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "hard", 1);
   assert(wfe_mcp_toolcall_action(SID, "read_file", NULL, 0) ==
          WFE_TC_ALLOW); /* not externalization */
   assert(wfe_mcp_toolcall_action("nobody", "pr.open", NULL, 0) ==
          WFE_TC_ALLOW); /* truly unbound */
   assert(wfe_mcp_toolcall_action(SID, "pr.open", NULL, 0) ==
          WFE_TC_DENY); /* pre-delivery externalize */
   assert(wfe_mcp_toolcall_action(SID, "git_push", NULL, 0) == WFE_TC_DENY);
   assert(audit_count(id) == 2); /* the two denials were audited */

   /* step 5: a DENY fills a TEMPLATED user message -- names the gate + work-item id,
    * never echoes the attempted tool name (pr.open). */
   {
      char msg[256] = "";
      assert(wfe_mcp_toolcall_action(SID, "pr.open", msg, sizeof msg) == WFE_TC_DENY);
      assert(msg[0]);
      assert(strstr(msg, "gate.deliver") != NULL);
      assert(strstr(msg, id) != NULL);        /* the work-item id is surfaced */
      assert(strstr(msg, "pr.open") == NULL); /* the attempted tool is NOT echoed */
      /* ALLOW clears the buffer */
      msg[0] = 'x';
      msg[1] = '\0';
      assert(wfe_mcp_toolcall_action(SID, "read_file", msg, sizeof msg) == WFE_TC_ALLOW);
      assert(msg[0] == '\0');
   }
   assert(audit_count(id) == 3); /* the message-check DENY added one more audit row */

   /* soft dial: warn + allow (still audited) */
   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "soft", 1);
   assert(wfe_mcp_toolcall_action(SID, "pr.open", NULL, 0) == WFE_TC_WARN);
   assert(audit_count(id) == 4);

   /* once delivered (gate.deliver -> accepted), the guard lifts */
   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "hard", 1);
   assert(db1_work_item_set_terminal(id, "accepted") == 0);
   assert(wfe_block_resolve(SID, &ctx) == 1 && ctx.delivered == 1);
   assert(wfe_mcp_toolcall_action(SID, "pr.open", NULL, 0) == WFE_TC_ALLOW);
   assert(audit_count(id) == 4); /* no new denial */

   /* a NON-enforced bound run is not externalization-guarded (delivered==accepted
    * would not be a sound gate proxy there, and it made no enforcement promise) */
   char id_u[80] = "";
   assert(wfe_work_item_create("u", "git@github.com:x/u.git", "docs/u.md", "interactive", id_u, err,
                               sizeof err) == 0);
   assert(db1_wfe_bind("sess-U", id_u, "advisory") == 0);
   assert(wfe_block_resolve("sess-U", &ctx) == 1 && ctx.enforced == 0);
   assert(wfe_mcp_toolcall_action("sess-U", "pr.open", NULL, 0) ==
          WFE_TC_ALLOW); /* hard, but not enforced */

   /* BOUND-but-unresolvable (binding references a vanished work-item): an
    * instrumentation failure on a known-bound session must fail CLOSED under hard,
    * NOT fail open. */
   assert(db1_wfe_bind("sess-G", "wi_ghost0000", "advisory") == 0);
   assert(wfe_block_resolve("sess-G", &ctx) == 0); /* cannot resolve */
   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "hard", 1);
   assert(wfe_mcp_toolcall_action("sess-G", "pr.open", NULL, 0) == WFE_TC_DENY); /* fail closed */
   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "soft", 1);
   assert(wfe_mcp_toolcall_action("sess-G", "pr.open", NULL, 0) ==
          WFE_TC_ALLOW); /* soft: observe */

   printf("ok\n");
   return 0;
}
