/* gw_orchestration_seam.c -- see gw_orchestration_seam.h. Pure (no I/O, no logging) so the
 * registry + runner stay unit-testable in isolation, mirroring gw_response_registry.c. The
 * fail-OPEN logging the header describes happens at the wire site, not in this core. */
#include "gw_orchestration_seam.h"

#include <string.h>

int gw_orchestration_registry_build(const gw_orch_hook_slot_t *slots, size_t n_slots,
                                    gw_orch_hook_t *out, size_t cap)
{
   if (!out || (!slots && n_slots))
      return -1;

   size_t k = 0;
   for (size_t i = 0; i < n_slots; i++)
   {
      if (!slots[i].enabled)
         continue;
      if (!slots[i].name || !slots[i].name[0] || !slots[i].fn)
         return -1;
      for (size_t j = 0; j < k; j++)
         if (strcmp(out[j].name, slots[i].name) == 0)
            return -1; /* duplicate enabled hook name */
      if (k >= cap)
         return -1; /* output overflow */
      out[k].fn = slots[i].fn;
      out[k].ud = slots[i].ud;
      out[k].name = slots[i].name;
      k++;
   }
   return (int)k;
}

gw_orch_result_t gw_orchestration_run(const gw_turn_snapshot_t *turn,
                                      const gw_turn_capabilities_t *caps,
                                      const gw_orch_hook_t *hooks, size_t n)
{
   gw_orch_result_t agg = {GW_ORCH_CONTINUE, NULL};

   if (!turn || !caps)
   {
      /* Cannot act on a missing turn. Fail OPEN: report it, but never abort the turn. */
      agg.status = GW_ORCH_FAIL;
      return agg;
   }
   if (!hooks)
   {
      if (n)
         agg.status = GW_ORCH_FAIL; /* inconsistent invocation; still fail-open */
      return agg;                   /* empty catalog -> clean CONTINUE */
   }

   int saw_fail = 0;
   for (size_t i = 0; i < n; i++)
   {
      if (!hooks[i].fn)
      {
         /* Malformed catalog entry (a hand-built array bypassing the registry check).
          * Fail OPEN: never dereference NULL, never block the turn; surface it below. */
         saw_fail = 1;
         continue;
      }
      gw_orch_result_t r = hooks[i].fn(turn, caps, hooks[i].ud);
      switch (r.status)
      {
      case GW_ORCH_COMPLETE:
         return r; /* definitive: short-circuit the turn, supersedes any prior soft FAIL */
      case GW_ORCH_SUSPEND:
         if (r.continuation)
            return r;  /* suspend with a continuation the caller now owns and frees */
         saw_fail = 1; /* SUSPEND without a continuation is malformed -> fail-open failure */
         break;
      case GW_ORCH_CONTINUE:
         break; /* run the next hook */
      case GW_ORCH_FAIL:
      default:
         /* FAIL (or an unknown status): fail-open -- a bad hook never blocks the turn, but
          * the failure is REMEMBERED so the caller can observe and log it. */
         saw_fail = 1;
         break;
      }
   }
   if (saw_fail)
      agg.status = GW_ORCH_FAIL; /* observable at the boundary; the turn still proceeds */
   return agg;
}
