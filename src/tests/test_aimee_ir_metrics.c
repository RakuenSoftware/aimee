/* test_aimee_ir_metrics.c -- active model-path counters: per-wire increment, totals,
 * names, reset. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <aimee/ir/aimee_ir_metrics.h>

int main(void)
{
   printf("aimee-ir-metrics: ");
   aimee_ir_metrics_reset();

   /* per-wire isolation */
   aimee_ir_metric_inc(AIMEE_IR_M_REASONING_OBSERVED, AIMEE_WIRE_ANTHROPIC);
   aimee_ir_metric_inc(AIMEE_IR_M_REASONING_OBSERVED, AIMEE_WIRE_ANTHROPIC);
   aimee_ir_metric_inc(AIMEE_IR_M_REASONING_OBSERVED, AIMEE_WIRE_OPENAI_CHAT);
   assert(aimee_ir_metric_get(AIMEE_IR_M_REASONING_OBSERVED, AIMEE_WIRE_ANTHROPIC) == 2);
   assert(aimee_ir_metric_get(AIMEE_IR_M_REASONING_OBSERVED, AIMEE_WIRE_OPENAI_CHAT) == 1);
   assert(aimee_ir_metric_get(AIMEE_IR_M_REASONING_OBSERVED, AIMEE_WIRE_RESPONSES) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_OBSERVED) == 3);

   /* other metrics unaffected */
   assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_INCOMPLETE) == 0);
   aimee_ir_metric_inc(AIMEE_IR_M_RESCUE_RECOVERIES, AIMEE_WIRE_ANTHROPIC);
   assert(aimee_ir_metric_total(AIMEE_IR_M_RESCUE_RECOVERIES) == 1);

   /* out-of-range guards + unknown-wire clamp (no crash) */
   aimee_ir_metric_inc(AIMEE_IR_M__COUNT, AIMEE_WIRE_ANTHROPIC); /* ignored */
   aimee_ir_metric_inc(AIMEE_IR_M_REASONING_INCOMPLETE, (aimee_wire_t)999); /* clamped */
   assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_INCOMPLETE) == 1);

   /* names are stable + distinct */
   assert(strcmp(aimee_ir_metric_name(AIMEE_IR_M_REASONING_OBSERVED), "ir_reasoning_observed") == 0);
   assert(strcmp(aimee_ir_metric_name(AIMEE_IR_M_REASONING_INCOMPLETE), "ir_reasoning_incomplete") ==
          0);
   assert(strcmp(aimee_ir_metric_name(AIMEE_IR_M_RESCUE_RECOVERIES), "ir_rescue_recoveries") == 0);

   /* reset clears everything */
   aimee_ir_metrics_reset();
   assert(aimee_ir_metric_total(AIMEE_IR_M_REASONING_OBSERVED) == 0);
   assert(aimee_ir_metric_total(AIMEE_IR_M_RESCUE_RECOVERIES) == 0);
   /* Every exposed observation has a stable name. */
   for (int m2 = 0; m2 < AIMEE_IR_M__COUNT; m2++)
   {
      const char *n = aimee_ir_metric_name((aimee_ir_metric_t)m2);
      assert(n && n[0]);
      assert(strncmp(n, "ir_", 3) == 0); /* stable, greppable prefix */
   }
   printf("ok\n");
   return 0;
}
