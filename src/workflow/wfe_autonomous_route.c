/* wfe_autonomous_route.c -- S4 autonomous-parity routing policy (PURE).
 * See wfe_autonomous_route.h for the design + roundtable rulings. */
#include "wfe_autonomous_route.h"

#include <string.h>

int wfe_autonomous_selectable(const char *id, int enforced)
{
   if (!id || !id[0] || !enforced)
      return 0;
   /* Defense in depth: even if a future YAML were mis-marked `enforced: true`,
    * never auto-select a known read-only lane. By I2 an enforced workflow cannot
    * actually be read-only (it must terminate in gate.deliver), so this is a
    * belt-and-suspenders guard, not the primary control. */
   if (strcmp(id, "converse") == 0 || strcmp(id, "research") == 0)
      return 0;
   return 1;
}

const char *wfe_autonomous_clamp(const char *router_id, int enforced, int *out_clamped)
{
   int sel = wfe_autonomous_selectable(router_id, enforced);
   if (out_clamped)
      *out_clamped = sel ? 0 : 1;
   return sel ? router_id : WFE_AUTONOMOUS_FLOOR;
}

const char *wfe_sweep_workflow_floor(void)
{
   return "manual-review";
}
