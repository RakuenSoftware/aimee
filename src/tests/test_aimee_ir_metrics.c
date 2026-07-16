/* test_aimee_ir_metrics.c -- shadow-mode counters: per-wire increment, totals,
 * names, reset. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee_ir_metrics.h"

int main(void)
{
   printf("aimee-ir-metrics: ");
   aimee_ir_metrics_reset();

   /* per-wire isolation */
   aimee_ir_metric_inc(AIMEE_IR_M_PARSE_FAIL, AIMEE_WIRE_ANTHROPIC);
   aimee_ir_metric_inc(AIMEE_IR_M_PARSE_FAIL, AIMEE_WIRE_ANTHROPIC);
   aimee_ir_metric_inc(AIMEE_IR_M_PARSE_FAIL, AIMEE_WIRE_OPENAI_CHAT);
   assert(aimee_ir_metric_get(AIMEE_IR_M_PARSE_FAIL, AIMEE_WIRE_ANTHROPIC) == 2);
   assert(aimee_ir_metric_get(AIMEE_IR_M_PARSE_FAIL, AIMEE_WIRE_OPENAI_CHAT) == 1);
   assert(aimee_ir_metric_get(AIMEE_IR_M_PARSE_FAIL, AIMEE_WIRE_RESPONSES) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_PARSE_FAIL) == 3);

   /* other metrics unaffected */
   assert(aimee_ir_metric_total(AIMEE_IR_M_REBUILD_MISMATCH) == 0);
   aimee_ir_metric_inc(AIMEE_IR_M_PASSTHROUGH, AIMEE_WIRE_ANTHROPIC);
   assert(aimee_ir_metric_total(AIMEE_IR_M_PASSTHROUGH) == 1);

   /* out-of-range guards + unknown-wire clamp (no crash) */
   aimee_ir_metric_inc(AIMEE_IR_M__COUNT, AIMEE_WIRE_ANTHROPIC); /* ignored */
   aimee_ir_metric_inc(AIMEE_IR_M_IR_PATH, (aimee_wire_t)999);   /* clamped to slot 0 */
   assert(aimee_ir_metric_total(AIMEE_IR_M_IR_PATH) == 1);

   /* names are stable + distinct */
   assert(strcmp(aimee_ir_metric_name(AIMEE_IR_M_REBUILD_MISMATCH), "ir_rebuild_mismatch_bytes") ==
          0);
   assert(strcmp(aimee_ir_metric_name(AIMEE_IR_M_PASSTHROUGH), "ir_passthrough") == 0);

   /* reset clears everything */
   aimee_ir_metrics_reset();
   assert(aimee_ir_metric_total(AIMEE_IR_M_PARSE_FAIL) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_PASSTHROUGH) == 0);
   /* Every metric must have a name, and the enum must be fully covered.
    * aimee_ir_metric_name is what the dashboard.metrics surface keys on; a metric
    * added without a name would silently publish as (null) or be dropped. These
    * counters exist to gate the legacy-deletion rollout — until they were exposed,
    * aimee_ir_metric_get had ZERO callers, so the fallback rate was unmeasurable
    * and "delete the legacy translators" could not be evidence-based. */
   for (int m2 = 0; m2 < AIMEE_IR_M__COUNT; m2++)
   {
      const char *n = aimee_ir_metric_name((aimee_ir_metric_t)m2);
      assert(n && n[0]);
      assert(strncmp(n, "ir_", 3) == 0); /* stable, greppable prefix */
   }
   /* The fallback signal the rollout depends on: a build failure must be visible
    * through the same read path the dashboard uses (total across wires). */
   aimee_ir_metrics_reset();
   aimee_ir_metric_inc(AIMEE_IR_M_BACKEND_BUILD_FAIL, AIMEE_WIRE_ANTHROPIC);
   aimee_ir_metric_inc(AIMEE_IR_M_BACKEND_BUILD_FAIL, AIMEE_WIRE_OPENAI_CHAT);
   assert(aimee_ir_metric_total(AIMEE_IR_M_BACKEND_BUILD_FAIL) == 2);
   assert(aimee_ir_metric_total(AIMEE_IR_M_IR_PATH) == 0);

   printf("ok\n");
   return 0;
}
