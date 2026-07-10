/* test_wfe_advance_exec.c -- S2 sub-slice 3 integration: drive the interactive
 * advance_request executor against a real in-memory DB1 + the real workflow
 * engine (stub executors so no delegate/git/panel is needed). Asserts the driver
 * resolves the binding, applies the CAS/replay decision, advances exactly one
 * engine step on OK, refuses stale/unbound, is idempotent on replay, and audits
 * every decision to the lifecycle log. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "db1.h"
#include "wfe_advance_exec.h"
#include "wfe_binding.h"
#include "wfe_engine.h"
#include "wfe_store.h"

/* linear non-enforced workflow; stub executors advance each block along `next`.
 * understand is exempt from the input-binding rule (it is the start); split /
 * implement each require an `in` binding, so they reference a.out. */
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
   char tmpl[] = "/tmp/wfe_adv_home_XXXXXX";
   char *dir = wfe_test_mkdtemp(tmpl);
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

/* status field of a result JSON */
static void status_of(const char *json, char *out, size_t n)
{
   out[0] = '\0';
   cJSON *r = cJSON_Parse(json);
   const cJSON *s = r ? cJSON_GetObjectItemCaseSensitive(r, "status") : NULL;
   if (s && cJSON_IsString(s))
      snprintf(out, n, "%s", s->valuestring);
   cJSON_Delete(r);
}

static const char *stage_now(const char *wi)
{
   static char buf[64];
   db1_work_item_t w;
   assert(db1_work_item_get(wi, &w) == 1);
   snprintf(buf, sizeof buf, "%s", w.current_stage);
   return buf;
}

int main(void)
{
   printf("wfe-advance-exec: ");
   setup_home();
   assert(db1_init(":memory:") == 0);
   wfe_reset_block_executors();
   wfe_register_stub_executors(); /* every block -> ADVANCED, follows `next` */

   char id[80] = "", err[256] = "";
   assert(wfe_work_item_create("t", "git@github.com:x/y.git", "docs/p.md", "interactive", id, err,
                               sizeof err) == 0);

   const char *SID = "sess-A";
   char out[1024], st[32];

   /* dial OFF -> inert regardless of binding */
   unsetenv("AIMEE_WORKFLOW_ENFORCE_STAGE");
   assert(wfe_advance_request_run(SID, "{\"work_item_id\":\"x\",\"observed_stage\":\"a\"}", out,
                                  sizeof out) == 0);
   status_of(out, st, sizeof st);
   assert(strcmp(st, "disabled") == 0);

   setenv("AIMEE_WORKFLOW_ENFORCE_STAGE", "advisory", 1);

   /* unbound session -> refused (dial on, no binding) */
   char args_a[256];
   snprintf(args_a, sizeof args_a, "{\"work_item_id\":\"%s\",\"observed_stage\":\"a\"}", id);
   assert(wfe_advance_request_run(SID, args_a, out, sizeof out) == 0);
   status_of(out, st, sizeof st);
   assert(strcmp(st, "unbound") == 0);

   /* bind the session, then a clean advance a -> b */
   assert(db1_wfe_bind(SID, id, "advisory") == 0);
   assert(strcmp(stage_now(id), "a") == 0);
   assert(wfe_advance_request_run(SID, args_a, out, sizeof out) == 0);
   status_of(out, st, sizeof st);
   assert(strcmp(st, "ok") == 0);
   assert(strcmp(stage_now(id), "b") == 0);

   /* stale: observed "a" but the work-item is now at "b" -> refuse, no advance */
   assert(wfe_advance_request_run(SID, args_a, out, sizeof out) == 0);
   status_of(out, st, sizeof st);
   assert(strcmp(st, "stale") == 0);
   assert(strcmp(stage_now(id), "b") == 0);

   /* a different session cannot advance this work-item (single-writer) */
   assert(wfe_advance_request_run("sess-B", args_a, out, sizeof out) == 0);
   status_of(out, st, sizeof st);
   assert(strcmp(st, "unbound") == 0);

   /* replay idempotency: a nonce'd advance b -> c, then the SAME nonce is a no-op */
   char args_b[256];
   snprintf(args_b, sizeof args_b,
            "{\"work_item_id\":\"%s\",\"observed_stage\":\"b\",\"nonce\":\"n1\"}", id);
   assert(wfe_advance_request_run(SID, args_b, out, sizeof out) == 0);
   status_of(out, st, sizeof st);
   assert(strcmp(st, "ok") == 0);
   assert(strcmp(stage_now(id), "c") == 0);
   /* retry the exact same call: observed "b" is now stale, but the nonce matches
    * the last applied advance -> REPLAY, not STALE, and no further advance */
   assert(wfe_advance_request_run(SID, args_b, out, sizeof out) == 0);
   status_of(out, st, sizeof st);
   assert(strcmp(st, "replay") == 0);
   assert(strcmp(stage_now(id), "c") == 0);

   /* Refusal rows must NOT pollute the replay nonce read-back. Reach a fresh
    * work-item at "a" via nonce "nA" (applied), record an intervening STALE refusal
    * that carries a DIFFERENT nonce "nZ", then retry the applied advance: it must
    * read as REPLAY (matching the last APPLIED nonce nA), not stale (which would
    * happen if the refusal's nonce nZ were mistaken for the last advance). */
   char id2[80] = "";
   assert(wfe_work_item_create("t", "git@github.com:x/z.git", "docs/q.md", "interactive", id2, err,
                               sizeof err) == 0);
   assert(db1_wfe_bind("sess-C", id2, "advisory") == 0);
   char applied[256], stale_z[256];
   snprintf(applied, sizeof applied,
            "{\"work_item_id\":\"%s\",\"observed_stage\":\"a\",\"nonce\":\"nA\"}", id2);
   snprintf(stale_z, sizeof stale_z,
            "{\"work_item_id\":\"%s\",\"observed_stage\":\"a\",\"nonce\":\"nZ\"}", id2);
   assert(wfe_advance_request_run("sess-C", applied, out, sizeof out) == 0); /* a->b, cas=ok nA */
   status_of(out, st, sizeof st);
   assert(strcmp(st, "ok") == 0);
   assert(wfe_advance_request_run("sess-C", stale_z, out, sizeof out) == 0); /* stale, nonce nZ */
   status_of(out, st, sizeof st);
   assert(strcmp(st, "stale") == 0);
   assert(wfe_advance_request_run("sess-C", applied, out, sizeof out) ==
          0); /* retry of the applied */
   status_of(out, st, sizeof st);
   assert(strcmp(st, "replay") == 0); /* nA still the last APPLIED nonce, not nZ */

   /* the driver audited its decisions (advance_req / advance-s2 events exist) */
   db1_lifecycle_event_t *ev = NULL;
   int ne = db1_lifecycle_event_list(id, &ev);
   int adv = 0, stale = 0;
   for (int i = 0; i < ne; i++)
      if (strcmp(ev[i].actor, "advance-s2") == 0)
      {
         if (strstr(ev[i].detail, "\"cas\":\"ok\""))
            adv++;
         if (strstr(ev[i].detail, "\"cas\":\"stale\""))
            stale++;
      }
   free(ev);
   assert(adv == 2);   /* a->b and b->c */
   assert(stale == 1); /* the one stale attempt */

   printf("ok\n");
   return 0;
}
