/* wfe_autonomous_route.c -- S4 autonomous-parity routing policy.
 *
 * The policy moved to the workflows module
 * (server-go/modules/workflows/autonomous.go) and is reached as bus stage
 * AIMEE_WORKFLOWS_STAGE_AUTONOMOUS_ROUTE. What stays here is the request and the
 * fail-closed reading of the answer.
 *
 * This decides which workflows an AUTONOMOUS run may auto-select, so it is a
 * safety control rather than a convenience. Every refusal -- an unreachable
 * module, an unreadable reply, an empty workflow name -- yields the floor, which
 * is a full-spine lane carrying review and a human gate. The failure direction is
 * therefore always MORE gated, never less; there is no path here that can hand an
 * autonomous run a lane the policy did not approve.
 *
 * See wfe_autonomous_route.h for the roundtable rulings this encodes.
 */
#include "wfe_autonomous_route.h"

#include "cJSON.h"
#include "headers/module_json_call.h"

#include <aimee/workflows/module_api.h>
#include <string.h>

/* Two short identifiers in, three small fields out. */
#define WFE_AUTONOMOUS_MAX_BODY (8u * 1024u)
/* On the routing path, so the deadline is short: a slow module clamps to the
 * floor rather than holding a submission open. */
#define WFE_AUTONOMOUS_TIMEOUT_MS 3000

/* One round trip. Fills `out` with the module's chosen workflow (bounded by
 * out_len) and returns 1 when the id was selectable, 0 otherwise. Any failure
 * reports "not selectable" and leaves `out` empty, which every caller below
 * turns into the floor. */
static int autonomous_route(const char *router_id, int enforced, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';

   cJSON *request = cJSON_CreateObject();
   if (!request)
      return 0;
   cJSON_AddStringToObject(request, "router_id", router_id ? router_id : "");
   cJSON_AddBoolToObject(request, "enforced", enforced ? 1 : 0);

   cJSON *reply = aimee_module_json_call(AIMEE_WORKFLOWS_EVENT_AUTONOMOUS_ROUTE,
                                         AIMEE_WORKFLOWS_STAGE_AUTONOMOUS_ROUTE, request,
                                         WFE_AUTONOMOUS_MAX_BODY, WFE_AUTONOMOUS_TIMEOUT_MS, NULL);
   if (!reply)
      return 0;

   const cJSON *selectable = cJSON_GetObjectItemCaseSensitive(reply, "selectable");
   const cJSON *workflow = cJSON_GetObjectItemCaseSensitive(reply, "workflow");
   int sel = cJSON_IsTrue(selectable);
   if (sel && out && out_len && cJSON_IsString(workflow) && workflow->valuestring[0])
   {
      size_t n = strlen(workflow->valuestring);
      if (n < out_len)
         memcpy(out, workflow->valuestring, n + 1);
      else
         sel = 0; /* a name we cannot hold is not a name we may route to */
   }
   else if (sel)
   {
      sel = 0; /* selectable with no usable workflow name is not an answer */
   }
   cJSON_Delete(reply);
   return sel;
}

int wfe_autonomous_selectable(const char *id, int enforced)
{
   char chosen[WFE_AUTONOMOUS_ID_MAX];
   return autonomous_route(id, enforced, chosen, sizeof chosen);
}

const char *wfe_autonomous_clamp(const char *router_id, int enforced, int *out_clamped)
{
   char chosen[WFE_AUTONOMOUS_ID_MAX];
   int sel = autonomous_route(router_id, enforced, chosen, sizeof chosen);
   if (out_clamped)
      *out_clamped = sel ? 0 : 1;
   /* Return the CALLER's pointer rather than the module's copy when the id was
    * accepted: the published contract is that a selectable id passes through
    * unchanged, and callers compare the result by pointer as well as by value. */
   return sel ? router_id : WFE_AUTONOMOUS_FLOOR;
}

const char *wfe_sweep_workflow_floor(void)
{
   /* A constant, not a decision: a round trip to be told a literal would buy
    * nothing. It is asserted equal to the module's SweepWorkflowFloor on both
    * sides so the two cannot drift apart unnoticed. */
   return WFE_SWEEP_WORKFLOW_FLOOR;
}
