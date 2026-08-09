/* test_wfe_autonomous_route.c -- the C side of the S4 autonomous routing policy.
 *
 * The POLICY moved to the workflows module
 * (server-go/modules/workflows/autonomous.go) and its rulings are pinned there,
 * by cases ported one-for-one from the block this file used to carry. What C
 * still owns, and what is covered here, is the request, the reading of the
 * answer, and the failure direction.
 *
 * That last part is the point. This decides which lanes an AUTONOMOUS run may
 * auto-select, so every way of not getting an answer -- module absent, transport
 * failure, malformed reply, a workflow name too long to hold -- must clamp to the
 * floor. A single path that returned "selectable" on a failure would let an
 * autonomous run take a lane the policy never approved.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wfe_autonomous_route.h"

#include "support/module_bus_stub.h"

#include <aimee/workflows/module_api.h>

int main(void)
{
   printf("wfe-autonomous-route: ");

   /* The floor is a full-spine enforced workflow, not the weaker "build", and
    * the sweep floor is the human gate. Both are mirrored module-side. */
   assert(strcmp(WFE_AUTONOMOUS_FLOOR, "managed-change") == 0);
   assert(strcmp(wfe_sweep_workflow_floor(), "manual-review") == 0);

   /* --- a selectable answer passes through --- */
   module_bus_stub_reply("{\"selectable\":true,\"workflow\":\"hotfix\",\"clamped\":false}");
   assert(wfe_autonomous_selectable("hotfix", 1) == 1);
   assert(module_bus_stub_last_event() == AIMEE_WORKFLOWS_EVENT_AUTONOMOUS_ROUTE);
   assert(module_bus_stub_last_stage() == AIMEE_WORKFLOWS_STAGE_AUTONOMOUS_ROUTE);

   int clamped = -1;
   const char *r = wfe_autonomous_clamp("hotfix", 1, &clamped);
   assert(strcmp(r, "hotfix") == 0 && clamped == 0);
   /* The contract is that a selectable id passes through UNCHANGED: callers may
    * compare the result against what they passed in. */
   const char *requested = "hotfix";
   assert(wfe_autonomous_clamp(requested, 1, NULL) == requested);

   /* --- a refusal clamps to the floor --- */
   module_bus_stub_reply("{\"selectable\":false,\"workflow\":\"managed-change\",\"clamped\":true}");
   assert(wfe_autonomous_selectable("research", 1) == 0);
   clamped = -1;
   r = wfe_autonomous_clamp("research", 1, &clamped);
   assert(strcmp(r, WFE_AUTONOMOUS_FLOOR) == 0 && clamped == 1);
   /* clamp tolerates a NULL out_clamped. */
   r = wfe_autonomous_clamp("research", 1, NULL);
   assert(strcmp(r, WFE_AUTONOMOUS_FLOOR) == 0);

   /* --- every way of not getting an answer clamps to the floor --- */
   const char *bad[] = {
       "{\"selectable\":true}",                   /* selectable with no workflow name */
       "{\"selectable\":true,\"workflow\":\"\"}", /* empty name */
       "{}",                                      /* nothing at all */
       "not json",                                /* unparseable */
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
   {
      module_bus_stub_reply(bad[i]);
      clamped = -1;
      r = wfe_autonomous_clamp("hotfix", 1, &clamped);
      assert(strcmp(r, WFE_AUTONOMOUS_FLOOR) == 0 && clamped == 1);
      assert(wfe_autonomous_selectable("hotfix", 1) == 0);
   }

   module_bus_stub_absent(); /* module not attached */
   clamped = -1;
   r = wfe_autonomous_clamp("hotfix", 1, &clamped);
   assert(strcmp(r, WFE_AUTONOMOUS_FLOOR) == 0 && clamped == 1);

   module_bus_stub_fail(AIMEE_MODULE_CALL_DEADLINE_EXCEEDED); /* module too slow */
   clamped = -1;
   r = wfe_autonomous_clamp("hotfix", 1, &clamped);
   assert(strcmp(r, WFE_AUTONOMOUS_FLOOR) == 0 && clamped == 1);

   /* A name longer than the clamp can hold must be treated as not selectable
    * rather than truncated, which would silently route to a DIFFERENT lane. */
   {
      char huge[WFE_AUTONOMOUS_ID_MAX + 64];
      int n = snprintf(huge, sizeof huge, "{\"selectable\":true,\"workflow\":\"");
      memset(huge + n, 'x', WFE_AUTONOMOUS_ID_MAX + 8);
      snprintf(huge + n + WFE_AUTONOMOUS_ID_MAX + 8,
               sizeof(huge) - (size_t)n - WFE_AUTONOMOUS_ID_MAX - 8, "\"}");
      module_bus_stub_reply(huge);
      clamped = -1;
      r = wfe_autonomous_clamp("hotfix", 1, &clamped);
      assert(strcmp(r, WFE_AUTONOMOUS_FLOOR) == 0 && clamped == 1);
   }

   printf("ok\n");
   return 0;
}
