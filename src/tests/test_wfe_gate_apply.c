/* test_wfe_gate_apply.c -- db1_work_item_gate_apply: the single guarded UPDATE
 * behind operator human-gate decisions. Proves the state-precondition guard
 * (current_stage + content_hash + pause_reason='pending_human') so a stale /
 * double / concurrent decision returns 0 (409) instead of corrupting state, and
 * that approve / terminal-reject / retry-loop-back each land correctly. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "db1.h"
#include "wfe_store.h"

/* Park a fresh work item at gate stage `stage` with content hash `hash`. */
static void make_parked(const char *id, const char *repo, const char *stage, const char *hash)
{
   assert(db1_work_item_create(id, repo, repo /*proposal_path*/, "build", "v1", stage,
                               "autonomous") == 0);
   assert(db1_work_item_set_stage(id, stage, hash) == 0);
   assert(db1_work_item_set_pause(id, "pending_human", "") == 0);
}

static void get(const char *id, db1_work_item_t *wi)
{
   assert(db1_work_item_get(id, wi) == 1);
}

int main(void)
{
   printf("wfe-gate-apply: ");
   assert(db1_init(":memory:") == 0);
   db1_work_item_t wi;

   /* --- approve: clears pause, keeps stage + active state. --- */
   make_parked("wi_a", "r/a", "g", "h1");
   assert(db1_work_item_gate_apply("wi_a", "g", "h1", NULL, NULL) == 1);
   get("wi_a", &wi);
   assert(wi.pause_reason[0] == '\0');         /* pause cleared */
   assert(strcmp(wi.current_stage, "g") == 0); /* stage unchanged */
   assert(strcmp(wi.state, "active") == 0);    /* not terminal */

   /* --- TOCTOU: a second identical apply now finds it not parked -> 0. --- */
   assert(db1_work_item_gate_apply("wi_a", "g", "h1", NULL, NULL) == 0);

   /* --- terminal reject: state=rejected, pause cleared. --- */
   make_parked("wi_t", "r/t", "g", "h1");
   assert(db1_work_item_gate_apply("wi_t", "g", "h1", NULL, "rejected") == 1);
   get("wi_t", &wi);
   assert(strcmp(wi.state, "rejected") == 0);
   assert(wi.pause_reason[0] == '\0');

   /* --- retry loop-back: current_stage moves to the on_fail target, pause cleared,
    *     still active. --- */
   make_parked("wi_r", "r/r", "g", "h1");
   assert(db1_work_item_gate_apply("wi_r", "g", "h1", "impl", NULL) == 1);
   get("wi_r", &wi);
   assert(strcmp(wi.current_stage, "impl") == 0);
   assert(wi.pause_reason[0] == '\0');
   assert(strcmp(wi.state, "active") == 0);

   /* --- guard: a stale content_hash is refused (optimistic-concurrency). --- */
   make_parked("wi_h", "r/h", "g", "h1");
   assert(db1_work_item_gate_apply("wi_h", "g", "WRONG", NULL, NULL) == 0);
   get("wi_h", &wi);
   assert(strcmp(wi.pause_reason, "pending_human") == 0); /* untouched */

   /* --- guard: a stale stage is refused. --- */
   assert(db1_work_item_gate_apply("wi_h", "wrongstage", "h1", NULL, NULL) == 0);
   get("wi_h", &wi);
   assert(strcmp(wi.pause_reason, "pending_human") == 0); /* still untouched */

   /* --- guard: a not-parked item (pause already cleared) is refused. --- */
   assert(db1_work_item_gate_apply("wi_h", "g", "h1", NULL, NULL) == 1);   /* clears it */
   assert(db1_work_item_gate_apply("wi_h", "g", "h1", "impl", NULL) == 0); /* now refused */

   printf("ok\n");
   return 0;
}
