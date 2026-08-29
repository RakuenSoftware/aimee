/* test_memory_lane_outcome.c — unit tests for per-lane recall outcome metrics.
 *
 * What is tested:
 *   memory_record_lane_outcome_metrics() attributes the SERVED window back to
 *   the lanes that produced it, so a lane that contributes candidates on every
 *   query and never places one can be told apart from a lane that carries the
 *   answer. The counters are measurement only: nothing in the recall path reads
 *   them back, and this test asserts the arithmetic, not a control decision.
 *
 *   - a lane's candidates are counted once per tagged row;
 *   - `served` counts only rows inside the served window, not the ranked tail;
 *   - a row tagged with two lanes counts for both;
 *   - a lane that contributed and placed nothing raises exactly one shutout;
 *   - a lane with no candidates emits no keys at all;
 *   - the route-qualified key appears only when a plan is supplied.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "aimee.h"
#include "modules/memory/memory_core_internal.h"

/* Capture sink for the runtime counters. db1_runtime_state_add_int is declared
 * `#pragma weak`, so defining it here binds memory_runtime_state_increment to
 * this test rather than to a live DB1. */
#define CAPTURE_MAX 64

static struct
{
   char key[160];
   int total;
} s_captured[CAPTURE_MAX];
static int s_captured_count;

int db1_runtime_state_add_int(const char *key, int delta, int *new_value_out);

int db1_runtime_state_add_int(const char *key, int delta, int *new_value_out)
{
   (void)new_value_out;
   for (int i = 0; i < s_captured_count; i++)
   {
      if (strcmp(s_captured[i].key, key) == 0)
      {
         s_captured[i].total += delta;
         return 0;
      }
   }
   assert(s_captured_count < CAPTURE_MAX);
   snprintf(s_captured[s_captured_count].key, sizeof(s_captured[0].key), "%s", key);
   s_captured[s_captured_count].total = delta;
   s_captured_count++;
   return 0;
}

static void reset_capture(void)
{
   s_captured_count = 0;
   memset(s_captured, 0, sizeof(s_captured));
}

/* -1 when the key was never emitted, which is distinct from an emitted zero. */
static int captured(const char *key)
{
   for (int i = 0; i < s_captured_count; i++)
      if (strcmp(s_captured[i].key, key) == 0)
         return s_captured[i].total;
   return -1;
}

static void test_served_window_attributed_to_lanes(void)
{
   reset_capture();

   /* Four candidates. 1 and 2 are served; 3 and 4 were ranked and dropped.
    * Row 2 carries two lanes, so it counts for both. The graph lane produced
    * only row 4, which did not survive. */
   memory_t matches[4];
   memset(matches, 0, sizeof(matches));
   matches[0].id = 1;
   matches[1].id = 2;
   matches[2].id = 3;
   matches[3].id = 4;

   memory_candidate_source_t stats[4] = {
       {1, MEM_SOURCE_SEMANTIC},
       {2, MEM_SOURCE_SEMANTIC | MEM_SOURCE_LEXICAL},
       {3, MEM_SOURCE_LEXICAL},
       {4, MEM_SOURCE_GRAPH},
   };

   memory_query_plan_t plan;
   memset(&plan, 0, sizeof(plan));
   plan.route = MEM_ROUTE_HYBRID;

   memory_record_lane_outcome_metrics(&plan, matches, 2, stats, 4);

   assert(captured("memory.query.lane.semantic.candidates") == 2);
   assert(captured("memory.query.lane.semantic.served") == 2);
   assert(captured("memory.query.lane.lexical.candidates") == 2);
   /* Row 2 is served and row 3 is not, so lexical places exactly one. */
   assert(captured("memory.query.lane.lexical.served") == 1);
   assert(captured("memory.query.lane.graph.candidates") == 1);
   assert(captured("memory.query.lane.graph.served") == 0);

   /* Only the lane that placed nothing raises a shutout. */
   assert(captured("memory.query.lane.graph.shutout") == 1);
   assert(captured("memory.query.lane.semantic.shutout") == -1);
   assert(captured("memory.query.lane.lexical.shutout") == -1);

   /* A lane with no candidates is silent rather than reported as zero. */
   assert(captured("memory.query.lane.alias.candidates") == -1);
   assert(captured("memory.query.lane.alias.served") == -1);

   assert(captured("memory.query.route.hybrid.lane.semantic.served") == 2);
   assert(captured("memory.query.route.hybrid.lane.graph.served") == 0);

   printf("  served window attributed to lanes: ok\n");
}

static void test_no_plan_omits_route_key(void)
{
   reset_capture();

   memory_t matches[1];
   memset(matches, 0, sizeof(matches));
   matches[0].id = 7;
   memory_candidate_source_t stats[1] = {{7, MEM_SOURCE_UNIT}};

   memory_record_lane_outcome_metrics(NULL, matches, 1, stats, 1);

   assert(captured("memory.query.lane.unit.served") == 1);
   assert(captured("memory.query.route.hybrid.lane.unit.served") == -1);

   printf("  route key omitted without a plan: ok\n");
}

static void test_empty_and_degenerate_inputs(void)
{
   reset_capture();
   memory_t matches[1];
   memset(matches, 0, sizeof(matches));
   matches[0].id = 1;
   memory_candidate_source_t stats[1] = {{1, MEM_SOURCE_LEXICAL}};

   /* No stats: nothing to attribute, so nothing is emitted. */
   memory_record_lane_outcome_metrics(NULL, matches, 1, NULL, 0);
   assert(s_captured_count == 0);
   memory_record_lane_outcome_metrics(NULL, matches, 1, stats, 0);
   assert(s_captured_count == 0);

   /* A served count of zero still records the candidates, and the shutout. */
   memory_record_lane_outcome_metrics(NULL, matches, 0, stats, 1);
   assert(captured("memory.query.lane.lexical.candidates") == 1);
   assert(captured("memory.query.lane.lexical.served") == 0);
   assert(captured("memory.query.lane.lexical.shutout") == 1);

   /* A negative served count is clamped rather than read off the end. */
   reset_capture();
   memory_record_lane_outcome_metrics(NULL, matches, -3, stats, 1);
   assert(captured("memory.query.lane.lexical.served") == 0);

   /* A NULL match array still accounts for the candidates it was told about. */
   reset_capture();
   memory_record_lane_outcome_metrics(NULL, NULL, 5, stats, 1);
   assert(captured("memory.query.lane.lexical.candidates") == 1);
   assert(captured("memory.query.lane.lexical.served") == 0);

   printf("  empty and degenerate inputs: ok\n");
}

int main(void)
{
   printf("test_memory_lane_outcome\n");
   test_served_window_attributed_to_lanes();
   test_no_plan_omits_route_key();
   test_empty_and_degenerate_inputs();
   printf("test_memory_lane_outcome: ok\n");
   return 0;
}
