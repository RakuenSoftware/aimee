/* test_gw_stage_registry.c -- Slice 7: the config-driven stage registry, proven
 * THROUGH the shared pipeline (gw_pipeline_run_request), not just via lookup. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "gateway_pipeline.h"
#include "gw_stage_registry.h"

static int g_spy;
static int spy_stage(gw_request_t *r, void *ud)
{
   (void)r;
   (void)ud;
   g_spy++;
   return 1;
}
static int other_stage(gw_request_t *r, void *ud)
{
   (void)r;
   (void)ud;
   return 0;
}

int main(void)
{
   gw_stage_t out[8];
   gw_request_t r;
   memset(&r, 0, sizeof(r));

   /* enabled module -> included, in order, and RUNS through the pipeline. */
   {
      gw_stage_slot_t slots[] = {
          {"memory", spy_stage, NULL, 1},
          {"other", other_stage, NULL, 1},
      };
      int n = gw_stage_registry_build(slots, 2, out, 8);
      assert(n == 2);
      assert(strcmp(out[0].name, "memory") == 0 && strcmp(out[1].name, "other") == 0);
      g_spy = 0;
      gw_pipeline_run_request(&r, out, (size_t)n);
      assert(g_spy == 1);
   }

   /* disabled module -> OMITTED, and does NOT run through the pipeline. */
   {
      gw_stage_slot_t slots[] = {
          {"memory", spy_stage, NULL, 0}, /* module removed via config */
          {"other", other_stage, NULL, 1},
      };
      int n = gw_stage_registry_build(slots, 2, out, 8);
      assert(n == 1);
      assert(strcmp(out[0].name, "other") == 0);
      g_spy = 0;
      gw_pipeline_run_request(&r, out, (size_t)n);
      assert(g_spy == 0);
   }

   /* duplicate ENABLED name -> configuration error. */
   {
      gw_stage_slot_t slots[] = {
          {"dup", spy_stage, NULL, 1},
          {"dup", other_stage, NULL, 1},
      };
      assert(gw_stage_registry_build(slots, 2, out, 8) == -1);
   }

   /* a disabled duplicate is fine (only the enabled set is checked). */
   {
      gw_stage_slot_t slots[] = {
          {"m", spy_stage, NULL, 1},
          {"m", other_stage, NULL, 0},
      };
      assert(gw_stage_registry_build(slots, 2, out, 8) == 1);
   }

   /* NULL fn / empty name in an enabled slot -> error. */
   {
      gw_stage_slot_t bad_fn[] = {{"x", NULL, NULL, 1}};
      assert(gw_stage_registry_build(bad_fn, 1, out, 8) == -1);
      gw_stage_slot_t bad_name[] = {{"", spy_stage, NULL, 1}};
      assert(gw_stage_registry_build(bad_name, 1, out, 8) == -1);
   }

   /* output overflow -> error (fail closed, no partial run). */
   {
      gw_stage_slot_t slots[] = {
          {"a", spy_stage, NULL, 1},
          {"b", spy_stage, NULL, 1},
      };
      assert(gw_stage_registry_build(slots, 2, out, 1) == -1);
   }

   printf("ok\n");
   return 0;
}
