/* test_wfe_bind_ingress.c -- S2 binding seam: the idempotent interactive bind
 * (real DB1 + router + engine, stub executors). Asserts: only enforced-routed
 * turns bind, dial-off is inert, binding is idempotent per session, and a bind
 * lifecycle event is recorded. */
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
   /* a SECOND enforced workflow so a bound-session turn can route elsewhere
    * (scope-add). Same shape as mc, distinct name. */
   snprintf(path, sizeof path, "%s/mc2.yaml", wf);
   f = fopen(path, "wb");
   assert(f);
   {
      const char *p = strstr(WF_MC, "\n");
      fputs("name: mc2", f);  /* replace the "name: mc" line */
      fputs(p ? p : "\n", f); /* + the rest of the workflow verbatim */
   }
   fclose(f);
   setenv("AIMEE_HOME", dir, 1);
   char repo[600];
   snprintf(repo, sizeof repo, "%s/repo", dir);
   mkdir(repo, 0755);
   setenv("AIMEE_WORKFLOW_REPO", repo, 1);
}

static int binding_wi(const char *sid, char *out, size_t n)
{
   return db1_wfe_binding_get(sid, out, n, NULL, 0);
}

int main(void)
{
   printf("wfe-bind-ingress: ");

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

   /* reject-scope-add surfacing (inc 3a): a bound session whose turn routes to a
    * DIFFERENT enforced workflow ("use mc2 ...") stays bound to mc (scope-add
    * rejected) and a scope_add_held event is recorded; a same-workflow turn does
    * NOT surface one. */
   assert(wfe_bind_interactive(SID, "use mc2 also fix X", NULL) == 1); /* still bound to mc */
   assert(binding_wi(SID, wi, sizeof wi) == 1 && strcmp(wi, first_wi) == 0);
   ev = NULL;
   ne = db1_lifecycle_event_list(first_wi, &ev);
   int held = 0;
   for (int i = 0; i < ne; i++)
      if (strcmp(ev[i].kind, "scope_add_held") == 0 && strcmp(ev[i].actor, "bind-s2") == 0)
         held++;
   free(ev);
   assert(held == 1);
   /* dedup: re-pivoting to the SAME held workflow does NOT add another event */
   assert(wfe_bind_interactive(SID, "use mc2 and again", NULL) == 1);
   ev = NULL;
   ne = db1_lifecycle_event_list(first_wi, &ev);
   int held_dup = 0;
   for (int i = 0; i < ne; i++)
      if (strcmp(ev[i].kind, "scope_add_held") == 0)
         held_dup++;
   free(ev);
   assert(held_dup == 1);                                             /* still one, not two */
   assert(wfe_bind_interactive(SID, "use mc keep going", NULL) == 1); /* same workflow */
   ev = NULL;
   ne = db1_lifecycle_event_list(first_wi, &ev);
   int held2 = 0;
   for (int i = 0; i < ne; i++)
      if (strcmp(ev[i].kind, "scope_add_held") == 0)
         held2++;
   free(ev);
   assert(held2 == 1); /* unchanged: same-workflow turn surfaces nothing */

   /* resume-after-reclaim (step 6 inc 2): make the lease stale, reclaim it (unbinds
    * SID; the work-item persists), then a fresh enforced turn RESUMES the same
    * work-item (create collides on interactive/<sid> -> by-proposal lookup) rather
    * than losing the work or refusing. */
   assert(db1_wfe_lease_renew(SID, -60) == 0);                    /* force stale */
   assert(db1_wfe_lease_reclaim_stale() == 1);                    /* reclaim -> SID unbound */
   assert(binding_wi(SID, wi, sizeof wi) == 0);                   /* unbound */
   assert(wfe_bind_interactive(SID, "use mc resume", NULL) == 1); /* resumes */
   assert(binding_wi(SID, wi, sizeof wi) == 1);
   assert(strcmp(wi, first_wi) == 0); /* SAME work-item */
   db1_work_item_t *items2 = NULL;
   assert(db1_work_item_list(&items2) == 1); /* no duplicate created */
   free(items2);
   /* the resume was audited as "resume" (not a fresh "bind") */
   ev = NULL;
   ne = db1_lifecycle_event_list(first_wi, &ev);
   int resumes = 0;
   for (int i = 0; i < ne; i++)
      if (strcmp(ev[i].actor, "bind-s2") == 0 && strcmp(ev[i].kind, "resume") == 0)
         resumes++;
   free(ev);
   assert(resumes == 1);

   /* a session id long enough to truncate the proposal path is REFUSED (so it can
    * never alias onto -- and hijack -- another session's work-item) */
   char longsid[200];
   memset(longsid, 'a', sizeof longsid - 1);
   longsid[sizeof longsid - 1] = '\0';
   assert(wfe_bind_interactive(longsid, "use mc long", NULL) == 0);
   assert(binding_wi(longsid, wi, sizeof wi) == 0);

   /* empty session id -> never binds */
   assert(wfe_bind_interactive("", "use mc x", NULL) == 0);

   /* bind-health detector: enforced routes with no INTERVENING bind -> WARN. */
   wfe_bind_health_reset();
   wfe_bind_health_note_enforced_route();
   wfe_bind_health_note_enforced_route();
   assert(wfe_bind_health_warned() == 0); /* below threshold */
   wfe_bind_health_note_enforced_route(); /* 3rd since last bind */
   assert(wfe_bind_health_warned() == 1); /* inert path detected */

   /* a working path (each route followed by a bind) never accumulates -> no warn */
   wfe_bind_health_reset();
   for (int i = 0; i < 5; i++)
   {
      wfe_bind_health_note_enforced_route();
      wfe_bind_health_note_bind();
   }
   assert(wfe_bind_health_warned() == 0);

   /* REGRESSION after working: a bind re-arms, then routes without binds warn AGAIN */
   wfe_bind_health_note_bind(); /* rearm */
   assert(wfe_bind_health_warned() == 0);
   wfe_bind_health_note_enforced_route();
   wfe_bind_health_note_enforced_route();
   wfe_bind_health_note_enforced_route();
   assert(wfe_bind_health_warned() == 1); /* regression re-detected */

   printf("ok\n");
   return 0;
}
