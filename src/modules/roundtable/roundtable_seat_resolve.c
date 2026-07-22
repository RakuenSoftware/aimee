/* roundtable_seat_resolve.c: see roundtable_seat_resolve.h. */
#include "roundtable_seat_resolve.h"

#include <string.h>

int rt_seat_is_random(const char *model)
{
   if (!model || !model[0])
      return 1; /* unset seat -> "any role-capable agent", never a fail */
   return strcmp(model, RT_SEAT_RANDOM) == 0;
}

rt_seat_resolve_t rt_resolve_seat_model(agent_config_t *cfg, const char *model, const char *role,
                                        const char *const used[], int nused, int *out_idx)
{
   if (!cfg || !role || !role[0] || !out_idx)
      return RT_SEAT_INVALID;
   *out_idx = -1;

   if (rt_seat_is_random(model))
   {
      int idx = delegate_pick_for_role(cfg, role, used, nused);
      if (idx < 0)
         return RT_SEAT_RANDOM_EXHAUSTED;
      *out_idx = idx;
      return RT_SEAT_OK;
   }

   /* Pinned: the EXACT model, or nothing — no substitution. */
   int idx = agent_pick_named_for_role(cfg, model, role);
   if (idx < 0)
      return RT_SEAT_PINNED_UNAVAILABLE;
   *out_idx = idx;
   return RT_SEAT_OK;
}
