/* test_gw_response_registry.c -- Slice 1 of the response seam: the response-stage
 * registry + runner, proven THROUGH the runner (not just lookup). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "gw_response_registry.h"

static int g_ran_spy, g_ran_after;
static gw_response_stage_result_t spy(gw_response_ctx_t *c, void *ud)
{
   (void)c;
   (void)ud;
   g_ran_spy++;
   gw_response_stage_result_t r = {GW_RSTAGE_OK, 2};
   return r;
}
static gw_response_stage_result_t after(gw_response_ctx_t *c, void *ud)
{
   (void)c;
   (void)ud;
   g_ran_after++;
   gw_response_stage_result_t r = {GW_RSTAGE_OK, 0};
   return r;
}
static gw_response_stage_result_t rejecter(gw_response_ctx_t *c, void *ud)
{
   (void)c;
   (void)ud;
   gw_response_stage_result_t r = {GW_RSTAGE_REJECT, 0};
   return r;
}

int main(void)
{
   gw_response_stage_t out[8];
   gw_response_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));

   /* enabled module runs through the runner, in order; OK interventions sum. */
   {
      gw_response_stage_slot_t slots[] = {
          {"governance", spy, NULL, 1},
          {"after", after, NULL, 1},
      };
      int n = gw_response_registry_build(slots, 2, out, 8);
      assert(n == 2 && strcmp(out[0].name, "governance") == 0);
      g_ran_spy = g_ran_after = 0;
      gw_response_stage_result_t res = gw_response_pipeline_run(&ctx, out, (size_t)n);
      assert(res.status == GW_RSTAGE_OK && res.interventions == 2);
      assert(g_ran_spy == 1 && g_ran_after == 1);
   }

   /* disabled module is OMITTED and does not run. */
   {
      gw_response_stage_slot_t slots[] = {
          {"governance", spy, NULL, 0},
          {"after", after, NULL, 1},
      };
      int n = gw_response_registry_build(slots, 2, out, 8);
      assert(n == 1 && strcmp(out[0].name, "after") == 0);
      g_ran_spy = 0;
      gw_response_pipeline_run(&ctx, out, (size_t)n);
      assert(g_ran_spy == 0);
   }

   /* REJECT stops the pipeline: later stages do NOT run, status propagates
    * (fail-closed: caller must not emit). */
   {
      gw_response_stage_slot_t slots[] = {
          {"gov", rejecter, NULL, 1},
          {"after", after, NULL, 1},
      };
      int n = gw_response_registry_build(slots, 2, out, 8);
      assert(n == 2);
      g_ran_after = 0;
      gw_response_stage_result_t res = gw_response_pipeline_run(&ctx, out, (size_t)n);
      assert(res.status == GW_RSTAGE_REJECT && g_ran_after == 0);
   }

   /* build errors: duplicate enabled name, NULL fn, empty name, overflow. */
   {
      gw_response_stage_slot_t dup[] = {{"d", spy, NULL, 1}, {"d", after, NULL, 1}};
      assert(gw_response_registry_build(dup, 2, out, 8) == -1);
      gw_response_stage_slot_t bad[] = {{"x", NULL, NULL, 1}};
      assert(gw_response_registry_build(bad, 1, out, 8) == -1);
      gw_response_stage_slot_t noname[] = {{"", spy, NULL, 1}};
      assert(gw_response_registry_build(noname, 1, out, 8) == -1);
      gw_response_stage_slot_t two[] = {{"a", spy, NULL, 1}, {"b", spy, NULL, 1}};
      assert(gw_response_registry_build(two, 2, out, 1) == -1);
   }

   /* a disabled duplicate is fine (only enabled set checked). */
   {
      gw_response_stage_slot_t slots[] = {{"m", spy, NULL, 1}, {"m", after, NULL, 0}};
      assert(gw_response_registry_build(slots, 2, out, 8) == 1);
   }

   /* runner fails closed on a NULL ctx. */
   {
      gw_response_stage_t none[1];
      gw_response_stage_result_t res = gw_response_pipeline_run(NULL, none, 0);
      assert(res.status == GW_RSTAGE_ERROR);
   }

   printf("ok\n");
   return 0;
}
