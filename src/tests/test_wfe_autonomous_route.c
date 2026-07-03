/* test_wfe_autonomous_route.c -- S4 autonomous-parity routing policy (pure).
 * Locks the roundtable rulings (2026-07-03): managed-change floor, full-spine
 * selectable set (enforced flag), read-only lanes never auto-selected, sweep
 * pinned to the human-gate floor. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wfe_autonomous_route.h"

int main(void)
{
   printf("wfe-autonomous-route: ");

   /* The floor is a full-spine enforced workflow, not the weaker "build". */
   assert(strcmp(WFE_AUTONOMOUS_FLOOR, "managed-change") == 0);

   /* selectable == enforced && not a known read-only lane && non-empty id. */
   assert(wfe_autonomous_selectable("managed-change", 1) == 1);
   assert(wfe_autonomous_selectable("hotfix", 1) == 1);
   /* enforced:false lanes (build / anything pre-gate.deliver) are NOT auto-selectable. */
   assert(wfe_autonomous_selectable("build", 0) == 0);
   /* read-only lanes are rejected even if mis-marked enforced (defense in depth). */
   assert(wfe_autonomous_selectable("converse", 1) == 0);
   assert(wfe_autonomous_selectable("research", 1) == 0);
   assert(wfe_autonomous_selectable("converse", 0) == 0);
   /* degenerate inputs fail closed. */
   assert(wfe_autonomous_selectable(NULL, 1) == 0);
   assert(wfe_autonomous_selectable("", 1) == 0);
   assert(wfe_autonomous_selectable("managed-change", 0) == 0); /* enforced flag off */

   /* clamp: a selectable id passes through unchanged, not clamped. */
   int clamped = -1;
   const char *r = wfe_autonomous_clamp("hotfix", 1, &clamped);
   assert(strcmp(r, "hotfix") == 0 && clamped == 0);

   /* clamp: a read-only / non-enforced / unknown id is lifted to the floor. */
   clamped = -1;
   r = wfe_autonomous_clamp("research", 0, &clamped);
   assert(strcmp(r, WFE_AUTONOMOUS_FLOOR) == 0 && clamped == 1);
   clamped = -1;
   r = wfe_autonomous_clamp("build", 0, &clamped);
   assert(strcmp(r, WFE_AUTONOMOUS_FLOOR) == 0 && clamped == 1);
   clamped = -1;
   r = wfe_autonomous_clamp(NULL, 0, &clamped);
   assert(strcmp(r, WFE_AUTONOMOUS_FLOOR) == 0 && clamped == 1);

   /* clamp tolerates a NULL out_clamped. */
   r = wfe_autonomous_clamp("managed-change", 1, NULL);
   assert(strcmp(r, "managed-change") == 0);

   /* sweep floor is the human gate, and it is NOT an auto-selectable lane
    * (unvetted candidates can never reach an auto-executing workflow). */
   assert(strcmp(wfe_sweep_workflow_floor(), "manual-review") == 0);
   assert(wfe_autonomous_selectable(wfe_sweep_workflow_floor(), 0) == 0);

   printf("ok\n");
   return 0;
}
