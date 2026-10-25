/* Canonical advisory routing and autonomous workflow audit.
 * Routing uses admitted canonical text and remains deterministic on the turn
 * path; this adapter never starts a separate model execution. */
#include "router_advise.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h" /* MAX_PATH_LEN etc. — must precede the agent headers */
#include "cJSON.h"
#include "ingress_preinject.h" /* Canonical ingress turn/session identity. */
#include "db1_client/interaction_events.h"
#include "wfe_autonomous_route.h" /* S4: clamp/floor policy */
#include "wfe_bind_ingress.h"
#include "wfe_enforce.h"
#include "wfe_router.h"
#include "db1_client/wfe_store.h" /* db1_lifecycle_event_add -- S4 route audit */

/* The decision + logging CORE: catalog -> prefilter -> decide -> advisory ie
 * record (+ the S2 enforce record when the dial is on). It runs NO LLM classifier
 * and NEVER mutates the request, so it is cheap and safe to call at the universal
 * gateway seam on every turn (primary AND delegate). Returns the prefilter outcome
 * (or -1 on a bad/empty catalog / bad args). `corr_id` is the admitted ingress
 * turn correlation id. */
static wfe_prefilter_outcome_t router_advise_log(const char *corr_id, const char *message)
{
   if (!corr_id || !corr_id[0] || !message || !message[0])
      return (wfe_prefilter_outcome_t)-1;

   wfe_router_catalog_t cat;
   char err[256];
   if (wfe_router_catalog_load(&cat, err, sizeof err) != 0)
      return (wfe_prefilter_outcome_t)-1; /* invalid/empty catalog -> fail closed */

   char mid[WFE_ROUTER_ID_LEN], reason[96];
   wfe_prefilter_outcome_t pf =
       wfe_router_prefilter(message, &cat, mid, sizeof mid, reason, sizeof reason);

   /* Immediate advisory decision (classifier=NULL -> latency-neutral): prefilter
    * result, or the read-only default for a DEFER. */
   wfe_route_decision_t d;
   wfe_router_decide(message, &cat, NULL, &d);
   char payload[512];
   wfe_router_advisory_payload(&d, pf, 0 /* not sampled */, -1.0 /* classifier not run inline */,
                               payload, sizeof payload);
   db1_interaction_event_record(corr_id, ie_event_type_name(IE_GUARDRAIL_DECISION), "router-s1",
                                payload, "advisory");

   /* S2: if the enforcement dial is on AND the routed workflow is enforced, log
    * the advisory enforce decision. The dial is env-gated and default-OFF (unset
    * -> WFE_ENFORCE_OFF), so this is inert until an operator opts in. Binding +
    * tool-strip build on this; here we prove the dial + enforced-detection fire on
    * the live provider path. Never blocks the turn. */
   wfe_enforce_stage_t estage = wfe_enforce_stage_parse(getenv("AIMEE_WORKFLOW_ENFORCE_STAGE"));
   if (estage != WFE_ENFORCE_OFF)
   {
      const wfe_router_wf_t *w = wfe_router_find(&cat, d.workflow_id);
      if (w && w->enforced)
      {
         char ep[256];
         snprintf(ep, sizeof ep, "{\"workflow\":\"%s\",\"enforced\":1,\"stage\":\"%s\"}",
                  d.workflow_id, wfe_enforce_stage_name(estage));
         db1_interaction_event_record(corr_id, ie_event_type_name(IE_GUARDRAIL_DECISION),
                                      "enforce-s2", ep, "advisory");
      }
   }
   return pf;
}

void router_advise_ir(const aimee_request_t *request)
{
   if (!request)
      return;
   for (int i = request->n_messages - 1; i >= 0; i--)
   {
      const aimee_message_t *message = &request->messages[i];
      if (!message->role || strcmp(message->role, "user"))
         continue;
      size_t length = 0;
      for (int j = 0; j < message->n_blocks; j++)
         if (message->blocks[j].type == AIMEE_BLK_TEXT && message->blocks[j].text)
            length += strlen(message->blocks[j].text) + 1;
      if (!length)
         return; /* Tool-result continuation is not a fresh routing request. */
      char *query = calloc(length, 1);
      if (!query)
         return;
      for (int j = 0; j < message->n_blocks; j++)
         if (message->blocks[j].type == AIMEE_BLK_TEXT && message->blocks[j].text)
         {
            if (query[0])
               strcat(query, "\n");
            strcat(query, message->blocks[j].text);
         }
      const char *turn = ingress_preinject_turn_id();
      router_advise_log(turn && turn[0] ? turn : "ingress", query);
      const char *session = ingress_preinject_session_id();
      if (session && session[0])
         wfe_bind_interactive(session, query, NULL);
      free(query);
      return;
   }
}

/* 8-hex FNV-1a of `s`: an audit-replay correlation tag (NOT a security digest). */
static void s4_proposal_tag(const char *s, char *out, size_t n)
{
   unsigned int h = 2166136261u;
   for (; s && *s; s++)
      h = (h ^ (unsigned char)*s) * 16777619u;
   snprintf(out, n, "%08x", h);
}

void router_autonomous_pick(const char *text, char *out_wf, size_t wf_n, char *out_src,
                            size_t src_n, char *out_raw, size_t raw_n, char *out_tag, size_t tag_n,
                            int *out_clamped)
{
   if (out_raw && raw_n)
      out_raw[0] = '\0';
   if (out_tag && tag_n)
      s4_proposal_tag(text, out_tag, tag_n);
   if (out_clamped)
      *out_clamped = 1;

   wfe_router_catalog_t cat;
   char cerr[256];
   if (wfe_router_catalog_load(&cat, cerr, sizeof cerr) != 0)
   {
      /* Catalog invalid/unavailable -> fail to the safe full-spine floor, never
       * to the weaker "build". */
      snprintf(out_wf, wf_n, "%s", WFE_AUTONOMOUS_FLOOR);
      if (out_src && src_n)
         snprintf(out_src, src_n, "cat-error");
      return;
   }
   /* classifier NULL: deterministic + low-latency intake, and `text` is untrusted
    * (no LLM prompt-injection surface). Every selectable lane carries the review
    * spine, so a shaped proposal can at worst pick among spine lanes. */
   wfe_route_decision_t d;
   wfe_router_decide(text, &cat, NULL, &d);
   const wfe_router_wf_t *wf = wfe_router_find(&cat, d.workflow_id);
   const char *chosen = wfe_autonomous_clamp(d.workflow_id, wf ? wf->enforced : 0, out_clamped);
   snprintf(out_wf, wf_n, "%s", chosen);
   if (out_raw && raw_n)
      snprintf(out_raw, raw_n, "%s", d.workflow_id);
   if (out_src && src_n)
      snprintf(out_src, src_n, "%s",
               d.source == WFE_ROUTE_SRC_PREFILTER
                   ? "prefilter"
                   : (d.source == WFE_ROUTE_SRC_CLASSIFIER ? "classifier" : "default"));
}

void router_autonomous_audit(const char *work_item_id, const char *chosen, const char *src,
                             const char *raw, int clamped, const char *tag)
{
   cJSON *det = cJSON_CreateObject();
   if (!det)
      return;
   cJSON_AddStringToObject(det, "chosen", chosen ? chosen : "");
   cJSON_AddStringToObject(det, "router_raw", raw ? raw : "");
   cJSON_AddStringToObject(det, "source", src ? src : "");
   cJSON_AddBoolToObject(det, "clamped_to_floor", clamped ? 1 : 0);
   char *dj = cJSON_PrintUnformatted(det);
   db1_lifecycle_event_add(work_item_id, "route-s4", "autoroute", "dev-submit", dj ? dj : "{}",
                           tag ? tag : "", 0.0);
   free(dj);
   cJSON_Delete(det);
}
