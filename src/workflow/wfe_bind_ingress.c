/* wfe_bind_ingress.c -- see wfe_bind_ingress.h. */
#include "wfe_bind_ingress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "wfe_binding.h" /* db1_wfe_bind, db1_wfe_binding_get */
#include "wfe_enforce.h" /* the dial */
#include "wfe_engine.h"  /* wfe_work_item_create */
#include "wfe_router.h"  /* catalog + decide + find */
#include "wfe_store.h"   /* db1_lifecycle_event_add */

static void audit_bind(const char *wi, const char *workflow, const char *stage)
{
   cJSON *d = cJSON_CreateObject();
   if (!d)
      return;
   cJSON_AddStringToObject(d, "workflow", workflow ? workflow : "");
   cJSON_AddStringToObject(d, "stage", stage ? stage : "");
   char *s = cJSON_PrintUnformatted(d);
   cJSON_Delete(d);
   if (s)
      db1_lifecycle_event_add(wi, "", "bind", "bind-s2", s, "", 0);
   free(s);
}

int wfe_bind_interactive(const char *session_id, const char *message, const char *repo)
{
   if (!session_id || !session_id[0])
      return 0;

   /* Default-OFF: the dial gates binding creation entirely. */
   wfe_enforce_stage_t stage = wfe_enforce_stage_parse(getenv("AIMEE_WORKFLOW_ENFORCE_STAGE"));
   if (stage == WFE_ENFORCE_OFF)
      return 0;

   /* Idempotent: already bound -> reuse, create nothing. This short-circuits every
    * turn after the first, so the create path runs at most once per session. */
   char wi[80] = "";
   if (db1_wfe_binding_get(session_id, wi, sizeof wi, NULL, 0) == 1 && wi[0])
      return 1;

   if (!message || !message[0])
      return 0;

   /* Route the turn (pure prefilter+decide, no LLM). Only ENFORCED workflows bind;
    * converse/research/non-enforced turns stay unbound (a generic session). */
   wfe_router_catalog_t cat;
   char err[256];
   if (wfe_router_catalog_load(&cat, err, sizeof err) != 0)
      return 0;
   wfe_route_decision_t d;
   wfe_router_decide(message, &cat, NULL /* no classifier at bind time */, &d);
   const wfe_router_wf_t *w = wfe_router_find(&cat, d.workflow_id);
   if (!w || !w->enforced)
      return 0;

   /* Deterministic per-session proposal path so UNIQUE(repo, proposal_path) is a
    * stable backstop against a duplicate create. */
   char proposal[128];
   snprintf(proposal, sizeof proposal, "interactive/%s", session_id);
   char id[80] = "";
   if (wfe_work_item_create(d.workflow_id, repo ? repo : "", proposal, "interactive", id, err,
                            sizeof err) != 0)
      return 0; /* create failed (incl. a rare UNIQUE collision after an unbind) */

   /* Bind the session, stamping the dial stage (monotonic per session row). */
   if (db1_wfe_bind(session_id, id, wfe_enforce_stage_name(stage)) != 0)
      return 0;

   /* Start the sliding lease (step 6 watchdog); renewed on each applied advance. */
   db1_wfe_lease_renew(session_id, wfe_lease_ttl_secs());
   audit_bind(id, w->id, wfe_enforce_stage_name(stage));
   return 1;
}
