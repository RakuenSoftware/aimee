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

/* Nonce of the most recent driver-applied advance for this work-item ("" if none),
 * so a retried turn carrying the same nonce is recognized as an idempotent replay
 * rather than re-applied. */
static void last_advance_nonce(const char *wi, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   db1_lifecycle_event_t *ev = NULL;
   int ne = db1_lifecycle_event_list(wi, &ev); /* oldest first */
   for (int i = ne - 1; i >= 0; i--)
   {
      if (strcmp(ev[i].kind, ADV_KIND) != 0 || strcmp(ev[i].actor, ADV_ACTOR) != 0)
         continue;
      cJSON *d = cJSON_Parse(ev[i].detail);
      const cJSON *nc = d ? cJSON_GetObjectItemCaseSensitive(d, "nonce") : NULL;
      if (nc && cJSON_IsString(nc) && nc->valuestring && out && n)
         snprintf(out, n, "%s", nc->valuestring);
      cJSON_Delete(d);
      break;
   }
   free(ev);
}

/* Audit a driver decision on a bound work-item. detail carries the CAS outcome and
 * (on a clean advance) the nonce + from/to stages -- id-charset values only, never
 * the primary's prose. */
static void audit(const char *wi, const char *stage, const char *outcome,
                  const wfe_advance_args_t *a, const char *to_stage)
{
   char detail[400];
   if (a->have_nonce)
      snprintf(detail, sizeof detail,
               "{\"cas\":\"%s\",\"from\":\"%s\",\"to\":\"%s\",\"nonce\":\"%s\"}", outcome,
               stage ? stage : "", to_stage ? to_stage : "", a->nonce);
   else
      snprintf(detail, sizeof detail, "{\"cas\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}", outcome,
               stage ? stage : "", to_stage ? to_stage : "");
   db1_lifecycle_event_add(wi, stage ? stage : "", ADV_KIND, ADV_ACTOR, detail, "", 0);
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
      /* Audit the refusal when we have a work-item to attribute it to. */
      if (b == 1 && bound_wi[0])
         audit(bound_wi, actual_stage, wfe_advance_outcome_name(oc), &a, "");
      cJSON *r = result_obj(wfe_advance_outcome_name(oc), a.work_item_id);
      if (r && actual_stage[0])
         cJSON_AddStringToObject(r, "actual_stage", actual_stage);
      write_result(out, out_n, r);
      return 0;
   }

   /* OK: advance exactly one engine step under the engine's own invariants. The
    * driver never writes run-state / gate.deliver directly. */
   wfe_advance_result_t res;
   memset(&res, 0, sizeof res);
   char err[256] = "";
   if (wfe_engine_advance(bound_wi, &res, err, sizeof err) != 0)
   {
      audit(bound_wi, actual_stage, "engine_error", &a, "");
      write_result(out, out_n, result_obj("error", bound_wi));
      return 0;
   }

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
