/* test_wfe_bind_ingress.c -- S2 binding seam: the auth-token->sid parser (pure)
 * + the idempotent interactive bind (real DB1 + router + engine, stub executors).
 * Asserts: only enforced-routed turns bind, dial-off is inert, binding is
 * idempotent per session, and a bind lifecycle event is recorded. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db1.h"
#include "wfe_bind_ingress.h"
#include "wfe_binding.h"
#include "wfe_engine.h"
#include "wfe_store.h"

/* enforced managed workflow "mc" (valid manager shape w/ terminal gate.deliver). */
static const char *WF_MC = "name: mc\n"
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

static void setup_home(void)
{
   char tmpl[] = "/tmp/wfe_bind_home_XXXXXX";
   char *dir = mkdtemp(tmpl);
   assert(dir);
   char wf[512];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   char path[640];
   snprintf(path, sizeof path, "%s/mc.yaml", wf);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(WF_MC, f);
   fclose(f);
   setenv("AIMEE_HOME", dir, 1);
   char repo[600];
   snprintf(repo, sizeof repo, "%s/repo", dir);
   mkdir(repo, 0755);
   setenv("AIMEE_WORKFLOW_REPO", repo, 1);
}

static void test_parse(void)
{
   char sid[64];
   assert(wfe_session_id_from_auth("aimee-sess-1a2b3c4d", sid, sizeof sid) == 1);
   assert(strcmp(sid, "1a2b3c4d") == 0);
   assert(wfe_session_id_from_auth("Bearer aimee-sess-deadbeef", sid, sizeof sid) == 1);
   assert(strcmp(sid, "deadbeef") == 0);
   /* not an aimee-session token */
   assert(wfe_session_id_from_auth("sk-ant-whatever", sid, sizeof sid) == 0);
   assert(wfe_session_id_from_auth("aimee-local", sid, sizeof sid) == 0);
   assert(wfe_session_id_from_auth("", sid, sizeof sid) == 0);
   assert(wfe_session_id_from_auth(NULL, sid, sizeof sid) == 0);
   /* bad charset in the sid (injection guard) */
   assert(wfe_session_id_from_auth("aimee-sess-a/b", sid, sizeof sid) == 0);
   assert(wfe_session_id_from_auth("aimee-sess-", sid, sizeof sid) == 0);
}

static int binding_wi(const char *sid, char *out, size_t n)
{
   return db1_wfe_binding_get(sid, out, n, NULL, 0);
}

int main(void)
{
   printf("wfe-bind-ingress: ");
   test_parse();

   setup_home();
   assert(db1_init(":memory:") == 0);
   wfe_reset_block_executors();
   wfe_register_stub_executors();

   const char *SID = "1a2b3c4d";
   char wi[80] = "";

   /* dial OFF -> inert (no bind even for an enforced-routed message) */
   unsetenv("AIMEE_WORKFLOW_ENFORCE_STAGE");
   assert(wfe_bind_interactive(SID, "use mc fix the bug", NULL) == 0);
   assert(binding_wi(SID, wi, sizeof wi) == 0);

   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "advisory", 1);

   /* a non-enforced route (converse) does NOT bind */
   assert(wfe_bind_interactive(SID, "hello there", NULL) == 0);
   assert(binding_wi(SID, wi, sizeof wi) == 0);

   /* an enforced route binds; a work-item is created + the session bound */
   assert(wfe_bind_interactive(SID, "use mc fix the bug", NULL) == 1);
   assert(binding_wi(SID, wi, sizeof wi) == 1);
   assert(wi[0]);
   char first_wi[80];
   snprintf(first_wi, sizeof first_wi, "%s", wi);

   /* the bind was audited */
   db1_lifecycle_event_t *ev = NULL;
   int ne = db1_lifecycle_event_list(first_wi, &ev);
   int binds = 0;
   for (int i = 0; i < ne; i++)
      if (strcmp(ev[i].actor, "bind-s2") == 0 && strcmp(ev[i].kind, "bind") == 0)
         binds++;
   free(ev);
   assert(binds == 1);

   /* idempotent: a second enforced turn reuses the SAME work-item, creates nothing */
   assert(wfe_bind_interactive(SID, "use mc keep going", NULL) == 1);
   assert(binding_wi(SID, wi, sizeof wi) == 1);
   assert(strcmp(wi, first_wi) == 0);
   /* still exactly one work item overall */
   db1_work_item_t *items = NULL;
   int n_items = db1_work_item_list(&items);
   free(items);
   assert(n_items == 1);

   /* empty session id -> never binds */
   assert(wfe_bind_interactive("", "use mc x", NULL) == 0);

   printf("ok\n");
   return 0;
}
