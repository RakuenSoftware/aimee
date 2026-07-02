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

/* understand(READONLY) -> split(DELEGATE) -> implement(DELEGATE); stub executors. */
static const char *WF = "name: t\n"
                        "start: a\n"
                        "nodes:\n"
                        "  - id: a\n"
                        "    block: understand\n"
                        "    next: b\n"
                        "  - id: b\n"
                        "    block: split\n"
                        "    in:\n"
                        "      intent: a.out\n"
                        "    next: c\n"
                        "  - id: c\n"
                        "    block: implement\n"
                        "    in:\n"
                        "      plan: a.out\n";

static void setup_home(void)
{
   char tmpl[] = "/tmp/wfe_res_home_XXXXXX";
   char *dir = mkdtemp(tmpl);
   assert(dir);
   char wf[512];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   char path[600];
   snprintf(path, sizeof path, "%s/t.yaml", wf);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(WF, f);
   fclose(f);
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

   /* bound at understand: READONLY surface, not delivered, advanceable, stage a */
   assert(wfe_block_resolve(SID, &ctx) == 1);
   assert(ctx.bound == 1);
   assert(ctx.surface == WFE_SURFACE_READONLY);
   assert(ctx.delivered == 0);
   assert(ctx.advanceable == 1);
   assert(strcmp(ctx.stage, "a") == 0);

   /* --- the guard --- */
   unsetenv("AIMEE_WORKFLOW_ENFORCE_STAGE");
   assert(wfe_mcp_toolcall_action(SID, "pr.open") == WFE_TC_ALLOW); /* dial off */

   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "hard", 1);
   assert(wfe_mcp_toolcall_action(SID, "read_file") == WFE_TC_ALLOW);    /* not externalization */
   assert(wfe_mcp_toolcall_action("nobody", "pr.open") == WFE_TC_ALLOW); /* unbound */
   assert(wfe_mcp_toolcall_action(SID, "pr.open") == WFE_TC_DENY); /* pre-delivery externalize */
   assert(wfe_mcp_toolcall_action(SID, "git_push") == WFE_TC_DENY);
   assert(audit_count(id) == 2); /* the two denials were audited */

   /* soft dial: warn + allow (still audited) */
   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "soft", 1);
   assert(wfe_mcp_toolcall_action(SID, "pr.open") == WFE_TC_WARN);
   assert(audit_count(id) == 3);

   /* once delivered (gate.deliver -> accepted), the guard lifts */
   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "hard", 1);
   assert(db1_work_item_set_terminal(id, "accepted") == 0);
   assert(wfe_block_resolve(SID, &ctx) == 1 && ctx.delivered == 1);
   assert(wfe_mcp_toolcall_action(SID, "pr.open") == WFE_TC_ALLOW);
   assert(audit_count(id) == 3); /* no new denial */

   printf("ok\n");
   return 0;
}
