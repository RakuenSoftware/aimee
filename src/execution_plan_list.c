/* execution_plan_list.c: listing plans is two reads, not one.
 *
 * db1_execution_plan_list has always been db1_execution_plan_list_ids followed
 * by a get for each id. The ids are storage and cross the boundary; the loop
 * around them is composition and stays here.
 *
 * Keeping it whole would have meant one reply carrying twenty plans, and a plan
 * is a nested row of up to 32 steps each holding a 4KB output -- so the reply
 * for a list nobody scrolls would be several megabytes, almost all of it steps
 * the caller never looks at.
 */
#include <string.h>

#include "agent_tasks.h"
#include "execution_plans.h"

int db1_execution_plan_list(plan_t *out, int max)
{
   if (!out || max <= 0)
      return -1;

   int ids[64];
   if (max > (int)(sizeof ids / sizeof ids[0]))
      max = (int)(sizeof ids / sizeof ids[0]);
   int found = db1_execution_plan_list_ids(ids, max);
   if (found < 0)
      return -1;

   /* A plan that vanishes between the two reads is skipped rather than fatal,
      which is what the single-read version did when its get failed. */
   int n = 0;
   for (int at = 0; at < found; at++)
   {
      if (db1_execution_plan_get(ids[at], &out[n]) == 0)
         n++;
   }
   return n;
}
