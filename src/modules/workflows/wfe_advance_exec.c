/* wfe_advance_exec.c -- see wfe_advance_exec.h. The interactive driver. */
#include "wfe_advance_exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "wfe_advance.h"
#include "wfe_binding.h" /* db1_wfe_binding_get */
#include "wfe_engine.h"  /* wfe_engine_advance */
#include "wfe_enforce.h" /* dial */
#include "wfe_store.h"   /* db1_work_item_get, lifecycle events */

/* Audit actor + record kind for a driver-applied advance (distinct from the
 * engine's own "advance" event, so the nonce scan below never confuses them). */
#define ADV_ACTOR "advance-s2"
#define ADV_KIND  "advance_req"

static void write_result(char *out, size_t out_n, cJSON *r)
{
   if (out && out_n)
   {
      char *s = r ? cJSON_PrintUnformatted(r) : NULL;
      snprintf(out, out_n, "%s", s ? s : "{\"status\":\"error\"}");
      free(s);
   }
   cJSON_Delete(r);
}

static cJSON *result_obj(const char *status, const char *work_item_id)
{
   cJSON *r = cJSON_CreateObject();
   if (!r)
      return NULL;
   cJSON_AddStringToObject(r, "status", status);
   if (work_item_id && work_item_id[0])
      cJSON_AddStringToObject(r, "work_item_id", work_item_id);
   return r;
}

/* Nonce of the most recent driver-APPLIED (cas=ok) advance for this work-item ("" if
 * none), so a retried turn carrying the same nonce is recognized as an idempotent
 * replay rather than re-applied. Refusals (stale/unbound/...) are audited under the
 * same kind and carry the attempted nonce, so they MUST be skipped here -- otherwise
 * a refusal's nonce would pollute the read-back and a legitimate retry of an applied
 * advance could be mis-decided. Events are ORDER BY id ASC, so scanning from the tail
 * finds the most recent applied advance first. */
static void last_advance_nonce(const char *wi, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   db1_lifecycle_event_t *ev = NULL;
   int ne = db1_lifecycle_event_list(wi, &ev); /* oldest first (ORDER BY id ASC) */
   for (int i = ne - 1; i >= 0; i--)
   {
      if (strcmp(ev[i].kind, ADV_KIND) != 0 || strcmp(ev[i].actor, ADV_ACTOR) != 0)
         continue;
      cJSON *d = cJSON_Parse(ev[i].detail);
      const cJSON *cas = d ? cJSON_GetObjectItemCaseSensitive(d, "cas") : NULL;
      int applied =
          cas && cJSON_IsString(cas) && cas->valuestring && strcmp(cas->valuestring, "ok") == 0;
      if (applied)
      {
         const cJSON *nc = cJSON_GetObjectItemCaseSensitive(d, "nonce");
         if (nc && cJSON_IsString(nc) && nc->valuestring && out && n)
            snprintf(out, n, "%s", nc->valuestring);
      }
      cJSON_Delete(d);
      if (applied)
         break; /* most recent APPLIED advance found; refusals in between skipped */
   }
   free(ev);
}

/* Audit a driver decision on a bound work-item. detail carries the CAS outcome and
 * (on a clean advance) the nonce + from/to stages. Built via cJSON so any value is
 * properly escaped -- stage ids are validator-constrained today, but the audit trail
 * must not depend on that invariant to stay parseable (defense-in-depth; the primary's
 * prose is never included). */
static void audit(const char *wi, const char *stage, const char *outcome,
                  const wfe_advance_args_t *a, const char *to_stage)
{
   cJSON *d = cJSON_CreateObject();
   if (!d)
      return;
   cJSON_AddStringToObject(d, "cas", outcome);
   cJSON_AddStringToObject(d, "from", stage ? stage : "");
   cJSON_AddStringToObject(d, "to", to_stage ? to_stage : "");
   if (a->have_nonce)
      cJSON_AddStringToObject(d, "nonce", a->nonce);
   char *s = cJSON_PrintUnformatted(d);
   cJSON_Delete(d);
   if (s)
      db1_lifecycle_event_add(wi, stage ? stage : "", ADV_KIND, ADV_ACTOR, s, "", 0);
   free(s);
}

int wfe_advance_request_run(const char *session_id, const char *args_json, char *out, size_t out_n)
{
   if (out && out_n)
      out[0] = '\0';

   /* Default-OFF: no binding, no driver. Inert until an operator opts in. */
   if (wfe_enforce_stage_parse(getenv("AIMEE_WORKFLOW_ENFORCE_STAGE")) == WFE_ENFORCE_OFF)
   {
      write_result(out, out_n, result_obj("disabled", NULL));
      return 0;
   }

   wfe_advance_args_t a;
   if (wfe_advance_parse_args(args_json, &a) != 0)
   {
      write_result(out, out_n, result_obj("badargs", NULL));
      return 0;
   }

   /* Resolve the caller's binding from AUTHORITATIVE DB state; never trust a
    * client-supplied work-item beyond matching it against the bound one. */
   char bound_wi[WFE_ADVANCE_WI_LEN] = "";
   int b = db1_wfe_binding_get(session_id, bound_wi, sizeof bound_wi, NULL, 0);
   if (b < 0)
   {
      write_result(out, out_n, result_obj("error", NULL));
      return 0;
   }

   db1_work_item_t wi;
   memset(&wi, 0, sizeof wi);
   const char *actual_stage = "";
   const char *actual_state = "";
   char last_nonce[WFE_ADVANCE_NONCE_LEN] = "";
   int wi_found = 0;
   if (b == 1 && bound_wi[0])
   {
      int g = db1_work_item_get(bound_wi, &wi);
      if (g < 0)
      {
         write_result(out, out_n, result_obj("error", bound_wi));
         return 0;
      }
      if (g == 1)
      {
         wi_found = 1;
         actual_stage = wi.current_stage;
         actual_state = wi.state;
         last_advance_nonce(bound_wi, last_nonce, sizeof last_nonce);
      }
      /* g == 0: binding row references a vanished work-item -> decide() sees empty
       * state/stage and returns STALE (safe: never advances). */
   }

   wfe_advance_outcome_t oc =
       wfe_advance_decide(bound_wi, &a, actual_stage, actual_state, last_nonce);

   if (oc != WFE_ADV_OK)
   {
      /* Audit the refusal only when the work-item actually exists, so a binding
       * that references a vanished work-item does not leave an orphan audit row. */
      if (wi_found)
         audit(bound_wi, actual_stage, wfe_advance_outcome_name(oc), &a, "");
      cJSON *r = result_obj(wfe_advance_outcome_name(oc), a.work_item_id);
      if (r && actual_stage[0])
         cJSON_AddStringToObject(r, "actual_stage", actual_stage);
      write_result(out, out_n, r);
      return 0;
   }

   /* OK: advance exactly one engine step under the engine's own invariants. The
    * driver never writes run-state / gate.deliver directly.
    *
    * Concurrency contract: decide()'s CAS + this advance are not one atomic txn, so
    * two TRULY concurrent advances of the same work-item that both observed the same
    * stage could double-execute a node (wfe_engine_advance reads current_stage before
    * its own txn -- the same property the autonomous single-runner scheduler relies
    * on). That is prevented here by the single-writer session<->work-item binding plus
    * the per-turn snapshot (one advance decision per turn, serial per session). A
    * SEQUENTIAL retry is handled precisely: the applied nonce is audited before the
    * retry arrives, so decide() returns REPLAY. The structural per-work-item lease
    * that would make even concurrent advances atomic is the Q4 single-writer lease,
    * owned by sub-slice 6. */
   wfe_advance_result_t res;
   memset(&res, 0, sizeof res);
   char err[256] = "";
   if (wfe_engine_advance(bound_wi, &res, err, sizeof err) != 0)
   {
      audit(bound_wi, actual_stage, "engine_error", &a, "");
      write_result(out, out_n, result_obj("error", bound_wi));
      return 0;
   }

   /* Meaningful advance -> renew the sliding lease (step 6). Renewing ONLY here (not
    * on refusals/replays) is deliberate: trivial turns must not hold the lease. */
   db1_wfe_lease_renew(session_id, wfe_lease_ttl_secs());
   audit(bound_wi, actual_stage, "ok", &a, res.next_stage);

   cJSON *r = result_obj("ok", bound_wi);
   if (r)
   {
      cJSON_AddStringToObject(r, "from_stage", a.observed_stage);
      cJSON_AddStringToObject(r, "stage", res.next_stage);
      cJSON_AddStringToObject(r, "state", res.state);
      cJSON_AddBoolToObject(r, "terminal", res.terminal ? 1 : 0);
   }
   write_result(out, out_n, r);
   return 0;
}
