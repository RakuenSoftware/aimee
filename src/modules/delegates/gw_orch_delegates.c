/* gw_orch_delegates.c -- see gw_orch_delegates.h. The module is thin (mirrors
 * gw_response_run_governance): a toggle, one hook, and a wrapper that runs the single-hook
 * catalog through the seam. The coord-specific capability adapter that actually spawns lives
 * at the producer (server_coord_dispatcher.c); this module only decides whether the module is
 * enabled and drives the runner, so it stays I/O-free and unit-testable with a fake caps. */
#include "gw_orch_delegates.h"

#include <stdlib.h>
#include <strings.h> /* strcasecmp */

int gw_orch_delegates_enabled(void)
{
   const char *v = getenv("AIMEE_ORCH_DELEGATES");
   if (!v || !v[0])
      return 1;
   return !(strcasecmp(v, "0") == 0 || strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0 ||
            strcasecmp(v, "no") == 0);
}

/* Per-spawn args handed to the hook via `ud`: a coord delegate has no request cJSON, so the
 * turn snapshot cannot carry role/brief -- they ride the slot's user data instead. */
typedef struct
{
   const char *role;
   const char *brief;
} delegate_hook_args_t;

/* The delegates hook: spawn a delegate through the narrow capability handle, then continue.
 * The spawn's success/failure is recorded by the capability adapter in caps->ctx (the
 * producer's backing); the hook itself is fail-open and continues regardless. */
static gw_orch_result_t delegates_hook(const gw_turn_snapshot_t *turn,
                                       const gw_turn_capabilities_t *caps, void *ud)
{
   (void)turn;
   const delegate_hook_args_t *a = (const delegate_hook_args_t *)ud;
   gw_orch_result_t r = {GW_ORCH_CONTINUE, NULL};
   if (!a || !caps->spawn_delegate)
   {
      /* Nothing to spawn with: fail-open, surfaced by the runner as an aggregate FAIL. */
      r.status = GW_ORCH_FAIL;
      return r;
   }
   caps->spawn_delegate(caps->ctx, a->role, a->brief);
   return r;
}

int gw_orch_delegates_run(const gw_turn_capabilities_t *caps, const char *turn_id, const char *role,
                          const char *brief, int enabled)
{
   if (!caps)
      return -1;
   delegate_hook_args_t args = {role, brief};
   gw_orch_hook_slot_t slots[] = {
       {"delegates", delegates_hook, &args, enabled},
   };
   gw_orch_hook_t hooks[1];
   int n = gw_orchestration_registry_build(slots, sizeof(slots) / sizeof(slots[0]), hooks, 1);
   if (n <= 0)
      return -1; /* module disabled (0) or catalog error (<0): no spawn attempted */
   gw_turn_snapshot_t turn = {turn_id ? turn_id : "delegate-spawn", NULL, NULL, NULL};
   gw_orchestration_run(&turn, caps, hooks, (size_t)n);
   return 0;
}
