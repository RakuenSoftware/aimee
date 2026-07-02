/* test_wfe_binding.c -- S2 session<->work-item binding (DB1): bind, idempotent
 * re-bind (monotonic stage), single-writer conflict, get, unbind + reclaim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "db1.h"
#include "wfe_binding.h"
#include "wfe_store.h" /* lifecycle_event list — assert the reclaim audit */

int main(void)
{
   printf("wfe-binding: ");
   assert(db1_init(":memory:") == 0);

   char wi[80] = "", st[16] = "";
   /* not bound yet */
   assert(db1_wfe_binding_get("sess1", wi, sizeof wi, st, sizeof st) == 0);

   /* bind sess1 -> wi_A (advisory) */
   assert(db1_wfe_bind("sess1", "wi_A", "advisory") == 0);
   assert(db1_wfe_binding_get("sess1", wi, sizeof wi, st, sizeof st) == 1);
   assert(strcmp(wi, "wi_A") == 0 && strcmp(st, "advisory") == 0);

   /* idempotent re-bind of the same session: succeeds, stage NOT downgraded/changed */
   assert(db1_wfe_bind("sess1", "wi_A", "hard") == 0);
   assert(db1_wfe_binding_get("sess1", wi, sizeof wi, st, sizeof st) == 1);
   assert(strcmp(st, "advisory") == 0); /* monotonic: stamped once */

   /* single-writer: a DIFFERENT session cannot bind wi_A */
   assert(db1_wfe_bind("sess2", "wi_A", "advisory") == -2);
   /* but sess2 can bind a different work-item */
   assert(db1_wfe_bind("sess2", "wi_B", "advisory") == 0);

   /* bad args fail closed */
   assert(db1_wfe_bind(NULL, "wi_C", "off") == -1);
   assert(db1_wfe_bind("sess3", "", "off") == -1);

   /* unbind sess1 releases wi_A; a new session can then reclaim it */
   assert(db1_wfe_unbind("sess1") == 0);
   assert(db1_wfe_binding_get("sess1", wi, sizeof wi, st, sizeof st) == 0);
   assert(db1_wfe_bind("sess3", "wi_A", "soft") == 0); /* wi_A free -> reclaim ok */

   /* ---- sliding lease (step 6 watchdog) ---- */
   char exp[32] = "";
   /* a fresh bind carries no lease until renewed */
   assert(db1_wfe_lease_expiry_get("sess3", exp, sizeof exp) == 1);
   assert(exp[0] == '\0');
   assert(db1_wfe_lease_expiry_get("nobody", exp, sizeof exp) == 0); /* unbound */
   assert(db1_wfe_lease_renew("nobody", 3600) == -1); /* no binding -> -1 per contract */

   char stale[4][80];
   /* renew forward -> has an expiry, not stale */
   assert(db1_wfe_lease_renew("sess3", 3600) == 0);
   assert(db1_wfe_lease_expiry_get("sess3", exp, sizeof exp) == 1 && exp[0]);
   assert(db1_wfe_lease_stale_work_items(stale, 4) == 0);

   /* force a PAST expiry -> stale; the sweep query surfaces the work-item */
   assert(db1_wfe_lease_renew("sess3", -60) == 0);
   assert(db1_wfe_lease_stale_work_items(stale, 4) == 1);
   assert(strcmp(stale[0], "wi_A") == 0);

   /* renew forward again -> no longer stale */
   assert(db1_wfe_lease_renew("sess3", 3600) == 0);
   assert(db1_wfe_lease_stale_work_items(stale, 4) == 0);

   /* clearing the lease (ttl 0) -> never stale even while bound */
   assert(db1_wfe_lease_renew("sess3", 0) == 0);
   assert(db1_wfe_lease_expiry_get("sess3", exp, sizeof exp) == 1 && exp[0] == '\0');
   assert(db1_wfe_lease_stale_work_items(stale, 4) == 0);

   /* ---- reclaim (step 6 inc 2): a stale lease is unbound + audited, work-item
    * freed. sess3 is bound to wi_A; sess2 (wi_B) never leased -> not reclaimed. */
   assert(db1_wfe_lease_renew("sess3", -60) == 0); /* make sess3 stale */
   assert(db1_wfe_lease_reclaim_stale() == 1);
   assert(db1_wfe_binding_get("sess3", wi, sizeof wi, st, sizeof st) == 0); /* unbound */
   assert(db1_wfe_binding_get("sess2", wi, sizeof wi, st, sizeof st) == 1); /* untouched */
   db1_lifecycle_event_t *ev = NULL;
   int ne = db1_lifecycle_event_list("wi_A", &ev);
   int reclaimed_events = 0;
   for (int i = 0; i < ne; i++)
      if (strcmp(ev[i].kind, "lease_reclaimed") == 0 && strcmp(ev[i].actor, "watchdog-s2") == 0)
         reclaimed_events++;
   free(ev);
   assert(reclaimed_events == 1);
   assert(db1_wfe_lease_reclaim_stale() == 0); /* nothing stale now */

   printf("ok\n");
   return 0;
}
