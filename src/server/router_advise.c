/* router_advise.c -- S1 advisory router hook (see router_advise.h). Ties the pure
 * router (prefilter + decision over the enumerated catalog) to the interaction-
 * event log.
 *
 * Two records per DEFER turn that is sampled:
 *  - the immediate advisory decision (outcome "advisory") is logged inline, with
 *    classifier=NULL so it is latency-neutral (prefilter + the read-only default);
 *  - the SAMPLED LLM classifier runs in a DETACHED thread (never on the user's
 *    turn path) and logs a second "classifier" record with the model's routed id
 *    + wall-clock. Sampling (~1/N of DEFER turns, deterministic) keeps this off
 *    the vast majority of turns; it is telemetry only and binds nothing. */
#include "router_advise.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aimee.h" /* MAX_PATH_LEN etc. — must precede the agent headers */
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_types.h"
#include "cJSON.h"
#include "gateway_pipeline.h"  /* gw_request_t — the shared gateway seam */
#include "ingress_preinject.h" /* query-from-messages + per-turn id */
#include "interaction_events.h"
#include "wfe_autonomous_route.h" /* S4: clamp/floor policy */
#include "wfe_bind_ingress.h"
#include "wfe_enforce.h"
#include "wfe_router.h"
#include "wfe_store.h" /* db1_lifecycle_event_add -- S4 route audit */

/* ~1 in N DEFER turns get the LLM classifier telemetry call. */
#define ROUTER_SAMPLE_ONE_IN_N 5

/* deterministic per-message sample key (no turn counter needed). */
static int msg_sample_key(const char *s)
{
   unsigned int h = 2166136261u;
   for (; s && *s; s++)
      h = (h ^ (unsigned char)*s) * 16777619u;
   return (int)(h & 0x7fffffff);
}

typedef struct
{
   char session_id[128];
   char *message; /* owned; freed by the thread */
} classify_args_t;

/* Detached background classifier: never touches the user's turn latency. Runs the
 * cheap-role LLM classifier, allowlist-parses its reply, and logs a "classifier"
 * telemetry record. */
static void *classify_thread(void *arg)
{
   classify_args_t *a = (classify_args_t *)arg;
   wfe_router_catalog_t cat;
   char err[256];
   if (wfe_router_catalog_load(&cat, err, sizeof err) == 0)
   {
      char sys[2048];
      wfe_router_classify_prompt(&cat, sys, sizeof sys);
      agent_config_t acfg;
      memset(&acfg, 0, sizeof acfg);
      if (agent_load_config(&acfg) == 0)
      {
         agent_result_t res;
         memset(&res, 0, sizeof res);
         struct timespec t0 = {0, 0}, t1 = {0, 0};
         clock_gettime(CLOCK_MONOTONIC, &t0);
         /* role "summarize" routes to a cheap roster agent. 256 tokens: the reply
          * is just an id, but a REASONING roster agent burns tokens thinking
          * before it answers, and too tight a cap yields an empty response
          * (observed live on .254 with minimax at 32 tokens). */
         int rc = agent_run(&acfg, "summarize", sys, a->message, 256, &res);
         clock_gettime(CLOCK_MONOTONIC, &t1);
         double ms =
             (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1.0e6;

         char cbuf[WFE_ROUTER_ID_LEN] = "";
         const char *cid =
             (rc == 0 && res.response &&
              wfe_router_parse_classification(res.response, &cat, cbuf, sizeof cbuf) == 0)
                 ? cbuf
                 : NULL; /* rc!=0 / no id -> DEFER falls back to the read-only default */
         wfe_route_decision_t d;
         wfe_router_decide(a->message, &cat, cid, &d);
         char payload[512];
         wfe_router_advisory_payload(&d, WFE_PREFILTER_DEFER, 1 /* sampled */, ms, payload,
                                     sizeof payload);
         ie_record(a->session_id, IE_GUARDRAIL_DECISION, "router-s1", payload, "classifier");
         free(res.response);
      }
   }
   free(a->message);
   free(a);
   return NULL;
}

/* The decision + logging CORE: catalog -> prefilter -> decide -> advisory ie
 * record (+ the S2 enforce record when the dial is on). It runs NO LLM classifier
 * and NEVER mutates the request, so it is cheap and safe to call at the universal
 * gateway seam on every turn (primary AND delegate). Returns the prefilter outcome
 * (or -1 on a bad/empty catalog / bad args) so a caller may decide to sample.
 * `corr_id` is the ie correlation id -- a session id on the legacy path, or the
 * ingress turn id at the gateway seam. */
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
   ie_record(corr_id, IE_GUARDRAIL_DECISION, "router-s1", payload, "advisory");

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
         ie_record(corr_id, IE_GUARDRAIL_DECISION, "enforce-s2", ep, "advisory");
      }
   }
   return pf;
}

/* Gateway pipeline stage: THE unified seam. Every inbound provider request
 * (primary CLI via /v1/messages, delegates via /v1/chat/completions) runs this,
 * co-located with gw_stage_tool_policing. We extract THIS turn's user query the
 * same way the memory stage does (NULL on a tool-result continuation -> fires once
 * per user turn) and run the advisory decision keyed on the ingress turn id. The
 * sampled LLM classifier is deliberately NOT run here: it calls agent_run, which
 * would re-enter this very seam -> recursion/cost. Never mutates the request. */
int gw_stage_router(gw_request_t *r, void *ud)
{
   (void)ud;
   if (!r || !r->raw)
      return 0;
   char *query =
       ingress_preinject_query_from_messages(cJSON_GetObjectItemCaseSensitive(r->raw, "messages"));
   if (!query)
      return 0;
   const char *tid = ingress_preinject_turn_id();
   router_advise_log((tid && tid[0]) ? tid : "ingress", query);

   /* S2 binding seam: if this turn carries an aimee session id (recovered at HTTP
    * ingress from the primary provider's "aimee-sess-<sid>" auth token) and the
    * router picks an enforced workflow, create + bind a work-item so the dispatch
    * guard + advance driver can fire. Idempotent per session; default-OFF via the
    * dial. Non-primary turns (delegates) carry no aimee-session token -> no-op. */
   const char *bsid = ingress_preinject_session_id();
   if (bsid && bsid[0])
      wfe_bind_interactive(bsid, query, NULL);
   free(query);
   return 0;
}

void router_advise_turn(const char *session_id, const char *message)
{
   wfe_prefilter_outcome_t pf = router_advise_log(session_id, message);
   if ((int)pf < 0 || !message)
      return;

   /* Sampled LLM classifier telemetry, off the turn path (detached thread). */
   if (pf == WFE_PREFILTER_DEFER &&
       wfe_router_should_sample(session_id, msg_sample_key(message), ROUTER_SAMPLE_ONE_IN_N))
   {
      classify_args_t *a = (classify_args_t *)calloc(1, sizeof *a);
      if (a)
      {
         snprintf(a->session_id, sizeof a->session_id, "%s", session_id);
         a->message = message[0] ? strdup(message) : NULL;
         pthread_t th;
         if (a->message && pthread_create(&th, NULL, classify_thread, a) == 0)
            pthread_detach(th);
         else
         {
            free(a->message);
            free(a);
         }
      }
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
