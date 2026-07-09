/* test_wfe_submitter.c -- intake-auth store layer for POST /v1/dev/submit:
 * the per-principal count helpers and the atomic db1_work_item_submit_capped
 * (cap-check + create + submitter-bind + audit under one BEGIN IMMEDIATE).
 * Proves: the binding round-trips; the active count is per-principal and keyed
 * off NON-terminal-ness (a human-parked run still occupies a slot — the B3
 * pause/resume bypass is closed); a terminal transition frees the slot; the
 * concurrency + rate caps return their distinct codes; and the recent-window
 * arg guards hold. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h> /* free() for the event list */
#include <string.h>

#include "db1.h"
#include "wfe_store.h"

/* Submit via the real atomic path with generous caps (so the cap never trips
 * here); unique proposal_path per id to satisfy UNIQUE(repo, proposal_path). */
static void submit(const char *id, const char *who)
{
   char path[64];
   snprintf(path, sizeof path, "p/%s", id);
   assert(db1_work_item_submit_capped(id, "r/x", path, "build", "v1", "intake", who,
                                      1000 /*max_active*/, 1000 /*rate_max*/, 60) == 0);
}

int main(void)
{
   printf("wfe-submitter: ");
   assert(db1_init(":memory:") == 0);
   db1_work_item_t wi;

   /* --- atomic submit binds submitter + sets autonomous/active. --- */
   submit("wi_1", "alice");
   assert(db1_work_item_get("wi_1", &wi) == 1);
   assert(strcmp(wi.submitter, "alice") == 0);
   assert(strcmp(wi.mode, "autonomous") == 0);
   assert(strcmp(wi.state, "active") == 0);

   /* --- active count is scoped per-principal. --- */
   submit("wi_2", "alice");
   submit("wi_3", "alice");
   submit("wi_4", "bob");
   assert(db1_work_item_count_active_by_submitter("alice") == 3);
   assert(db1_work_item_count_active_by_submitter("bob") == 1);
   assert(db1_work_item_count_active_by_submitter("nobody") == 0);

   /* --- B3: a HUMAN-PARKED run still counts (cap can't be freed by pausing). --- */
   assert(db1_work_item_set_pause("wi_2", "pending_human", "") == 0);
   assert(db1_work_item_count_active_by_submitter("alice") == 3); /* still 3, not 2 */

   /* --- a terminal transition DOES free the slot. --- */
   assert(db1_work_item_set_terminal("wi_3", "accepted") == 0);
   assert(db1_work_item_count_active_by_submitter("alice") == 2);

   /* --- concurrency cap: at max_active the next submit returns 1, writes nothing. --- */
   assert(db1_work_item_submit_capped("wi_cc", "r/x", "p/wi_cc", "build", "v1", "intake", "alice",
                                      2 /*max_active, alice already has 2*/, 1000, 60) == 1);
   assert(db1_work_item_get("wi_cc", &wi) == 0); /* not created */

   /* --- rate cap: generous concurrency but rate_max already met -> returns 2. --- */
   assert(db1_work_item_count_recent_by_submitter("bob", 60) == 1);
   assert(db1_work_item_submit_capped("wi_rc", "r/x", "p/wi_rc", "build", "v1", "intake", "bob",
                                      1000, 1 /*rate_max, bob already has 1 recent*/, 60) == 2);
   assert(db1_work_item_get("wi_rc", &wi) == 0); /* not created */

   /* --- a cap value <= 0 disables that cap (submit succeeds despite the count). --- */
   assert(db1_work_item_submit_capped("wi_un", "r/x", "p/wi_un", "build", "v1", "intake", "bob",
                                      0 /*concurrency disabled*/, 0 /*rate disabled*/, 60) == 0);

   /* --- recent-window arg guards. --- */
   assert(db1_work_item_count_recent_by_submitter("alice", 0) == -1);  /* bad window */
   assert(db1_work_item_count_recent_by_submitter("alice", -5) == -1); /* bad window */

   /* --- the submit audit row is attributed to the principal. --- */
   db1_lifecycle_event_t *ev = NULL;
   int ne = db1_lifecycle_event_list("wi_1", &ev);
   int saw_submit = 0;
   for (int i = 0; i < ne; i++)
      if (strcmp(ev[i].kind, "submit") == 0 && strcmp(ev[i].actor, "alice") == 0)
         saw_submit = 1;
   free(ev);
   assert(saw_submit);

   /* --- foreach.workflow parent<->child linkage + terminal-state aggregation. --- */
   {
      assert(db1_work_item_create("par", "r/p", "p/par", "build", "v1", "slices", "autonomous") == 0);
      const char *kids[] = {"ch1", "ch2", "ch3", "ch4"};
      for (int i = 0; i < 4; i++)
      {
         char path[64];
         snprintf(path, sizeof path, "p/%s", kids[i]);
         assert(db1_work_item_create(kids[i], "r/p", path, "slice", "v1", "impl", "autonomous") == 0);
         assert(db1_work_item_set_parent(kids[i], "par") == 0);
      }
      /* the link round-trips */
      assert(db1_work_item_get("ch2", &wi) == 1);
      assert(strcmp(wi.parent_id, "par") == 0);
      /* a top-level run has no parent */
      assert(db1_work_item_get("par", &wi) == 1);
      assert(wi.parent_id[0] == '\0');
      /* aggregate: 4 children, none terminal yet */
      int total = -1, acc = -1, fail = -1;
      assert(db1_work_item_child_counts("par", &total, &acc, &fail) == 0);
      assert(total == 4 && acc == 0 && fail == 0);
      /* two slices merge (accepted); one rejected + one abandoned both count as failed
       * (a slice that will never merge). */
      assert(db1_work_item_set_terminal("ch1", "accepted") == 0);
      assert(db1_work_item_set_terminal("ch2", "accepted") == 0);
      assert(db1_work_item_set_terminal("ch3", "rejected") == 0);
      assert(db1_work_item_set_terminal("ch4", "abandoned") == 0);
      assert(db1_work_item_child_counts("par", &total, &acc, &fail) == 0);
      assert(total == 4 && acc == 2 && fail == 2);
      /* a parent with no children aggregates to zero (not an error) */
      assert(db1_work_item_child_counts("nobody", &total, &acc, &fail) == 0 && total == 0);
      /* set_parent on an unknown work item fails closed */
      assert(db1_work_item_set_parent("ghost", "par") == -1);
   }

   printf("ok\n");
   return 0;
}
