/* aimee_ir_metrics.c -- see aimee_ir_metrics.h. Atomic counters, [metric][wire]. */
#include "aimee_ir_metrics.h"

/* Sized off the LAST wire value, so adding a protocol widens the table
 * automatically. (Was AIMEE_WIRE_GEMINI + 1 until the Gemini wire was removed —
 * Gemini speaks the OpenAI shape, so it needs no protocol of its own.) */
#define N_WIRE (AIMEE_WIRE_RESPONSES + 1)

static long g_counts[AIMEE_IR_M__COUNT][N_WIRE];

static int wire_idx(aimee_wire_t w)
{
   int i = (int)w;
   return (i >= 0 && i < N_WIRE) ? i : 0; /* clamp unknown/out-of-range to slot 0 */
}

void aimee_ir_metric_inc(aimee_ir_metric_t m, aimee_wire_t frontend)
{
   if ((int)m < 0 || m >= AIMEE_IR_M__COUNT)
      return;
   __atomic_fetch_add(&g_counts[m][wire_idx(frontend)], 1, __ATOMIC_RELAXED);
}

long aimee_ir_metric_get(aimee_ir_metric_t m, aimee_wire_t frontend)
{
   if ((int)m < 0 || m >= AIMEE_IR_M__COUNT)
      return 0;
   return __atomic_load_n(&g_counts[m][wire_idx(frontend)], __ATOMIC_RELAXED);
}

long aimee_ir_metric_total(aimee_ir_metric_t m)
{
   if ((int)m < 0 || m >= AIMEE_IR_M__COUNT)
      return 0;
   long sum = 0;
   for (int w = 0; w < N_WIRE; w++)
      sum += __atomic_load_n(&g_counts[m][w], __ATOMIC_RELAXED);
   return sum;
}

const char *aimee_ir_metric_name(aimee_ir_metric_t m)
{
   switch (m)
   {
   case AIMEE_IR_M_PARSE_FAIL:
      return "ir_parse_failures";
   case AIMEE_IR_M_RENDER_FAIL:
      return "ir_render_failures";
   case AIMEE_IR_M_BACKEND_BUILD_FAIL:
      return "ir_backend_build_failures";
   case AIMEE_IR_M_BACKEND_PARSE_FAIL:
      return "ir_backend_parse_failures";
   case AIMEE_IR_M_REBUILD_MISMATCH:
      return "ir_rebuild_mismatch_bytes";
   case AIMEE_IR_M_STAGE_MUTATION:
      return "ir_stage_mutations";
   case AIMEE_IR_M_CACHE_CONTROL_LOST:
      return "ir_cache_control_lost";
   case AIMEE_IR_M_PASSTHROUGH:
      return "ir_passthrough";
   case AIMEE_IR_M_IR_PATH:
      return "ir_path";
   case AIMEE_IR_M__COUNT:
   default:
      return "unknown";
   }
}

void aimee_ir_metrics_reset(void)
{
   for (int m = 0; m < AIMEE_IR_M__COUNT; m++)
      for (int w = 0; w < N_WIRE; w++)
         __atomic_store_n(&g_counts[m][w], 0, __ATOMIC_RELAXED);
}
