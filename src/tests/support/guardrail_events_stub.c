/* guardrail_events_stub.c: guardrail events, counted, in memory.
 *
 * COUNTED FOR REAL, because one test depends on the round trip:
 * test_semantic_advisory_pre_tool_check takes a baseline, drives two tool
 * checks, and asserts prompt == before + 2 with warn, block and dry_run
 * unchanged.
 *
 * That assertion's subject is the GUARDRAIL -- that it classified both actions
 * as prompt and nothing else -- and the store is only the medium it is read
 * through. Counting here keeps the claim intact. A no-op insert with a zeroed
 * count would fail the assertion outright; a fixed count would pass it while
 * measuring nothing, which is worse.
 *
 * Bucketed by final_action, which is what the real 7-day query groups on. No
 * time window: nothing in this binary can age an event out within a run.
 */

#include <string.h>

#include "db1_client/guardrail_events.h"

static guardrail_event_counts_t g_counts;

int db1_guardrail_event_insert(const guardrail_event_t *e)
{
   if (!e)
   {
      return -1;
   }
   if (e->dry_run)
   {
      g_counts.dry_run++;
   }
   else if (strcmp(e->final_action, "warn") == 0)
   {
      g_counts.warn++;
   }
   else if (strcmp(e->final_action, "prompt") == 0)
   {
      g_counts.prompt++;
   }
   else if (strcmp(e->final_action, "block") == 0)
   {
      g_counts.block++;
   }
   return 0;
}

int db1_guardrail_event_counts_7d(guardrail_event_counts_t *out)
{
   if (!out)
   {
      return -1;
   }
   *out = g_counts;
   return 0;
}
