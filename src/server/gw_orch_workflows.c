/* gw_orch_workflows.c -- see gw_orch_workflows.h. Thin, like gw_orch_delegates: a toggle, one
 * hook, and a wrapper that runs the single-hook catalog through the seam. The workflow-specific
 * capability adapter that actually creates the run lives at the producer
 * (trigger_scheduler.c), so this module stays I/O-free and unit-testable with a fake caps. */
#include "gw_orch_workflows.h"

#include <stdlib.h>
#include <strings.h> /* strcasecmp */

int gw_orch_workflows_enabled(void)
{
   const char *v = getenv("AIMEE_ORCH_WORKFLOWS");
   if (!v || !v[0])
      return 1;
   return !(strcasecmp(v, "0") == 0 || strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0 ||
            strcasecmp(v, "no") == 0);
}

/* Per-dispatch args handed to the hook via `ud`: a background workflow dispatch has no request
 * cJSON, so the turn snapshot cannot carry lane/payload -- they ride the slot's user data. */
typedef struct
{
   const char *lane;
   const char *payload;
} workflow_hook_args_t;

/* The workflows hook: dispatch a workflow through the narrow capability handle, then continue.
 * The dispatch's success/failure is recorded by the capability adapter in caps->ctx (the
 * producer's backing); the hook itself is fail-open and continues regardless. */
static gw_orch_result_t workflows_hook(const gw_turn_snapshot_t *turn,
                                       const gw_turn_capabilities_t *caps, void *ud)
{
   (void)turn;
   const workflow_hook_args_t *a = (const workflow_hook_args_t *)ud;
   gw_orch_result_t r = {GW_ORCH_CONTINUE, NULL};
   if (!a || !caps->dispatch_workflow)
   {
      /* Nothing to dispatch with: fail-open, surfaced by the runner as an aggregate FAIL. */
      r.status = GW_ORCH_FAIL;
      return r;
   }
   caps->dispatch_workflow(caps->ctx, a->lane, a->payload);
   return r;
}

int gw_orch_workflows_run(const gw_turn_capabilities_t *caps, const char *turn_id, const char *lane,
                          const char *payload, int enabled)
{
   if (!caps)
      return -1;
   workflow_hook_args_t args = {lane, payload};
   gw_orch_hook_slot_t slots[] = {
       {"workflows", workflows_hook, &args, enabled},
   };
   gw_orch_hook_t hooks[1];
   int n = gw_orchestration_registry_build(slots, sizeof(slots) / sizeof(slots[0]), hooks, 1);
   if (n <= 0)
      return -1; /* module disabled (0) or catalog error (<0): no dispatch attempted */
   gw_turn_snapshot_t turn = {turn_id ? turn_id : "workflow-dispatch", NULL, NULL, NULL};
   gw_orchestration_run(&turn, caps, hooks, (size_t)n);
   return 0;
}
