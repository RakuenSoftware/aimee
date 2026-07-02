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

static void audit_bind(const char *wi, const char *workflow, const char *stage, int resumed)
{
   cJSON *d = cJSON_CreateObject();
   if (!d)
      return;
   cJSON_AddStringToObject(d, "workflow", workflow ? workflow : "");
   cJSON_AddStringToObject(d, "stage", stage ? stage : "");
   char *s = cJSON_PrintUnformatted(d);
   cJSON_Delete(d);
   /* "resume" when re-binding to an existing work-item after a lease reclaim. */
   if (s)
      db1_lifecycle_event_add(wi, "", resumed ? "resume" : "bind", "bind-s2", s, "", 0);
   free(s);
}

/* Reject-scope-add surfacing (consult #30): a session already bound to one work-item
 * cannot silently expand scope. If THIS turn routes to a DIFFERENT enforced workflow
 * ("also fix X"), the safe-S2 rule HOLDS the new intent -- the session must finish /
 * deliver its current work-item first. Record a templated "scope_add_held" event
 * (workflow ids only, no prose) so the rejection is observable; the binding is left
 * unchanged. Same-workflow / non-enforced turns are normal work and surface nothing. */
static void wfe_scope_add_surface(const char *wi, const char *message)
{
   if (!wi || !wi[0] || !message || !message[0])
      return;
   db1_work_item_t item;
   if (db1_work_item_get(wi, &item) != 1)
      return;
   wfe_router_catalog_t cat;
   char err[256];
   if (wfe_router_catalog_load(&cat, err, sizeof err) != 0)
      return;
   wfe_route_decision_t d;
   wfe_router_decide(message, &cat, NULL, &d);
   const wfe_router_wf_t *w = wfe_router_find(&cat, d.workflow_id);
   if (!w || !w->enforced || strcmp(d.workflow_id, item.workflow_name) == 0)
      return; /* not a scope-add: same workflow, or a non-enforced turn */

   char detail[192];
   snprintf(detail, sizeof detail, "{\"held\":\"%s\",\"current\":\"%s\"}", d.workflow_id,
            item.workflow_name);
   /* Dedup: surface once per DISTINCT held workflow, not once per turn -- a client
    * re-pivoting every turn must not grow the audit log unbounded. */
   db1_lifecycle_event_t *ev = NULL;
   int ne = db1_lifecycle_event_list(wi, &ev);
   int already = 0;
   for (int i = 0; i < ne && !already; i++)
      if (strcmp(ev[i].kind, "scope_add_held") == 0 && strcmp(ev[i].detail, detail) == 0)
         already = 1;
   free(ev);
   if (!already)
      db1_lifecycle_event_add(wi, "", "scope_add_held", "bind-s2", detail, "", 0);
}

int wfe_bind_interactive(const char *session_id, const char *message, const char *repo)
{
   if (!session_id || !session_id[0])
      return 0;

   /* Default-OFF: the dial gates binding creation entirely. */
   wfe_enforce_stage_t stage = wfe_enforce_stage_parse(getenv("AIMEE_WORKFLOW_ENFORCE_STAGE"));
   if (stage == WFE_ENFORCE_OFF)
      return 0;

   /* Opportunistic watchdog (step 6 inc 2): reclaim any idle-expired leases on each
    * enforced turn -- self-healing, no separate scheduler. The lease slides only on
    * a MEANINGFUL advance (not trivial turn traffic), so a session squatting a
    * work-item without progress eventually lapses and is reclaimed here (consult
    * #12). Cheap when nothing is stale (a bounded query returning no rows). */
   db1_wfe_lease_reclaim_stale();

   /* Idempotent: already bound -> reuse, create nothing. This short-circuits every
    * turn after the first, so the create path runs at most once per session. */
   char wi[80] = "";
   if (db1_wfe_binding_get(session_id, wi, sizeof wi, NULL, 0) == 1 && wi[0])
   {
      /* Bound already: reuse (create nothing). Surface a scope-add attempt so the
       * safe-S2 rejection is observable; the binding stays put. */
      wfe_scope_add_surface(wi, message);
      return 1;
   }

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
    * stable backstop against a duplicate create. Refuse on truncation: a truncated
    * path could alias two long same-prefix session ids onto ONE work-item, letting
    * one session resume/bind another's (aimee ids are short, but this must not
    * depend on that). */
   char proposal[128];
   int pr = snprintf(proposal, sizeof proposal, "interactive/%s", session_id);
   if (pr < 0 || (size_t)pr >= sizeof proposal)
      return 0;
   char id[80] = "";
   int resumed = 0;
   if (wfe_work_item_create(d.workflow_id, repo ? repo : "", proposal, "interactive", id, err,
                            sizeof err) != 0)
   {
      /* Create failed. The expected case is a UNIQUE(repo, proposal_path) collision
       * after the session's lease was reclaimed (step 6) -- the work-item still
       * exists, so RESUME it by binding to the existing id rather than losing the
       * work. Any other failure (bad workflow, DB fault) leaves id empty -> refuse. */
      if (db1_work_item_id_by_proposal(repo ? repo : "", proposal, id, sizeof id) != 1 || !id[0])
         return 0;
      resumed = 1;
   }

   /* Bind the session, stamping the dial stage (monotonic per session row). */
   if (db1_wfe_bind(session_id, id, wfe_enforce_stage_name(stage)) != 0)
      return 0;

   /* Start the sliding lease (step 6 watchdog); renewed on each applied advance. */
   db1_wfe_lease_renew(session_id, wfe_lease_ttl_secs());
   audit_bind(id, w->id, wfe_enforce_stage_name(stage), resumed);
   return 1;
}
