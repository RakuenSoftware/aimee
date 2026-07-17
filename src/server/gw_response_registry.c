/* gw_response_registry.c -- see gw_response_registry.h. */
#include "gw_response_registry.h"

#include <string.h>

int gw_response_registry_build(const gw_response_stage_slot_t *slots, size_t n_slots,
                               gw_response_stage_t *out, size_t cap)
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
            return -1; /* duplicate enabled stage name */
      if (k >= cap)
         return -1; /* output overflow */
      out[k].fn = slots[i].fn;
      out[k].ud = slots[i].ud;
      out[k].name = slots[i].name;
      k++;
   }
   return (int)k;
}

gw_response_stage_result_t gw_response_pipeline_run(gw_response_ctx_t *ctx,
                                                    const gw_response_stage_t *stages, size_t n)
{
   gw_response_stage_result_t agg = {GW_RSTAGE_OK, 0};
   if (!ctx || (!stages && n))
   {
      agg.status = GW_RSTAGE_ERROR; /* fail closed on a bad invocation */
      return agg;
   }
   for (size_t i = 0; i < n; i++)
   {
      gw_response_stage_result_t r = stages[i].fn(ctx, stages[i].ud);
      if (r.interventions > 0)
         agg.interventions += r.interventions;
      if (r.status != GW_RSTAGE_OK)
      {
         agg.status = r.status; /* stop; caller must not emit a reply */
         return agg;
      }
   }
   return agg;
}
