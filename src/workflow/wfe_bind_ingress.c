/* wfe_bind_ingress.c -- see wfe_bind_ingress.h. */
#include "wfe_bind_ingress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wfe_binding.h" /* db1_wfe_bind, db1_wfe_binding_get */
#include "wfe_enforce.h" /* the dial */
#include "wfe_engine.h"  /* wfe_work_item_create */
#include "wfe_router.h"  /* catalog + decide + find */
#include "wfe_store.h"   /* db1_lifecycle_event_add */

/* id-charset guard: [A-Za-z0-9_-], 1..cap-1 chars (the session-id mint format). */
static int id_ok(const char *s, size_t cap)
{
   if (!s || !s[0])
      return 0;
   size_t l = 0;
   for (const char *p = s; *p; p++, l++)
   {
      char c = *p;
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-'))
         return 0;
   }
   return l < cap;
}

int wfe_session_id_from_auth(const char *auth_value, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   if (!auth_value || !out || !n)
      return 0;
   const char *v = auth_value;
   if (strncmp(v, "Bearer ", 7) == 0)
      v += 7;
   size_t pfx = sizeof(WFE_SESSION_TOKEN_PREFIX) - 1;
   if (strncmp(v, WFE_SESSION_TOKEN_PREFIX, pfx) != 0)
      return 0;
   const char *sid = v + pfx;
   if (!id_ok(sid, n))
      return 0;
   snprintf(out, n, "%s", sid);
   return 1;
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

   char detail[128];
   snprintf(detail, sizeof detail, "{\"workflow\":\"%s\",\"stage\":\"%s\"}", w->id,
            wfe_enforce_stage_name(stage));
   db1_lifecycle_event_add(id, "", "bind", "bind-s2", detail, "", 0);
   return 1;
}
