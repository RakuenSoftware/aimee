/* gw_stage_registry.c -- see gw_stage_registry.h. */
#include "gw_stage_registry.h"

#include <string.h>

int gw_stage_registry_build(const gw_stage_slot_t *slots, size_t n_slots, gw_stage_t *out,
                            size_t cap)
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
