/* wfe_autonomy.c: the autonomy driver + human-only gate-override. */
#include "wfe_autonomy.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "wfe_store.h"
#include "wfe_approval.h"
#include "wfe_def.h"
#include "wfe_engine.h"
#include "wfe_iface.h"

/* Is this paused node a human gate the driver may auto-satisfy in autonomous
 * mode? (policy preauthorized, or optional:true). */
static int human_gate_auto_ok(const wfe_node_t *node)
{
   if (!node || node->block != WFE_BLK_GATE_HUMAN)
      return 0;
   const cJSON *policy =
       node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "policy") : NULL;
   const cJSON *optional =
       node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "optional") : NULL;
   if (policy && cJSON_IsString(policy) && strcmp(policy->valuestring, "preauthorized") == 0)
      return 1;
   /* accept optional written as bool/int/string (YAML emitters vary) */
   if (optional &&
       (cJSON_IsTrue(optional) || (cJSON_IsNumber(optional) && optional->valuedouble != 0) ||
        (cJSON_IsString(optional) && strcasecmp(optional->valuestring, "true") == 0)))
      return 1;
   return 0;
}

int wfe_autonomy_run(const char *work_item_id, char *err, size_t errlen)
{
   for (int i = 0; i < 10000; i++)
   {
      wfe_advance_result_t r;
      if (wfe_engine_advance(work_item_id, &r, err, errlen) != 0)
         return -1;
      if (r.terminal)
         return 0;
      if (r.last_status == WFE_STEP_FAILED)
         return 0;
      if (r.last_status != WFE_STEP_PENDING)
         continue; /* ADVANCED / LOOPED: keep going */

      /* parked. Only autonomous mode + an auto-OK human gate may proceed. */
      db1_work_item_t wi;
      if (db1_work_item_get(work_item_id, &wi) != 1)
         return 0;
      if (strcmp(wi.mode, "autonomous") != 0)
         return 0; /* interactive: park */

      char ferr[256];
      wfe_def_t *def = wfe_load_workflow(wi.workflow_name, ferr, sizeof ferr);
      const wfe_node_t *node = def ? wfe_def_node(def, wi.current_stage) : NULL;
      int auto_ok = human_gate_auto_ok(node);
      if (def)
         wfe_def_free(def);
      if (!auto_ok)
         return 0; /* roundtable-degraded / non-preauthorized human gate: park */

      /* preauthorized human gate: record a signed approval + clear the pause so
       * the next advance passes it. (This is a standing user authorization, not
       * a forged approval.) */
      if (wfe_approval_record(work_item_id, wi.current_stage, wi.content_hash,
                              "autonomous-preauth") != 0)
      {
         snprintf(err, errlen, "autonomy: could not record preauth approval at '%s'",
                  wi.current_stage);
         return -1; /* surface the failure; do not silently park forever */
      }
      db1_lifecycle_event_add(work_item_id, wi.current_stage, "resume", "autonomous",
                              "preauthorized", wi.content_hash, 0);
      db1_work_item_clear_pause(work_item_id);
   }
   snprintf(err, errlen, "autonomy run exceeded step bound");
   return -1;
}

int wfe_gate_override(const char *work_item_id, const char *gate, const char *actor,
                      const char *reason, char *err, size_t errlen)
{
   /* the override is attributed to the authenticated caller; the call site (CLI /
    * v1) must pass the real principal — it is bound into the approval MAC. */
   const char *act = (actor && actor[0]) ? actor : "user";
   db1_work_item_t wi;
   if (db1_work_item_get(work_item_id, &wi) != 1)
   {
      snprintf(err, errlen, "unknown work item");
      return -1;
   }
   /* override is only meaningful on a PARKED, active work item — refuse to
    * record an override approval against an active/terminal item (otherwise a
    * caller could mint approvals + burn the cap on a work item not at a gate). */
   if (strcmp(wi.state, "active") != 0 || !wi.pause_reason[0])
   {
      snprintf(err, errlen, "work item is not parked at a gate (state=%s pause=%s)", wi.state,
               wi.pause_reason);
      return -1;
   }
   /* override only the gate the work item is actually parked at, and never a
    * roundtable gate (override is a human-gate affordance — fail closed). */
   if (gate && strcmp(gate, wi.current_stage) != 0)
   {
      snprintf(err, errlen, "gate '%s' is not the current stage '%s'", gate, wi.current_stage);
      return -1;
   }
   {
      char ferr[256];
      wfe_def_t *def = wfe_load_workflow(wi.workflow_name, ferr, sizeof ferr);
      const wfe_node_t *node = def ? wfe_def_node(def, wi.current_stage) : NULL;
      int is_roundtable = node && node->block == WFE_BLK_GATE_ROUNDTABLE;
      if (def)
         wfe_def_free(def);
      if (is_roundtable)
      {
         snprintf(err, errlen, "cannot override a roundtable gate ('%s')", wi.current_stage);
         return -1;
      }
   }
   if (wi.override_count >= WFE_MAX_OVERRIDES)
   {
      db1_work_item_set_terminal(work_item_id, "rejected");
      db1_lifecycle_event_add(work_item_id, gate, "rejected", act, "override cap reached", "", 0);
      return 1;
   }
   /* an override is a recorded human approval bound to the current artifact +
    * the authenticated actor (the MAC covers actor). */
   if (wfe_approval_record(work_item_id, gate, wi.content_hash, act) != 0)
   {
      snprintf(err, errlen, "could not sign override (approval key?)");
      return -1;
   }
   db1_work_item_inc_override(work_item_id);
   db1_lifecycle_event_add(work_item_id, gate, "override", act, reason ? reason : "",
                           wi.content_hash, 0);
   db1_work_item_clear_pause(work_item_id);
   return 0;
}
