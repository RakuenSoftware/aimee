/* Canonical model-path observations, counted atomically per frontend wire. */
#ifndef DEC_AIMEE_IR_METRICS_H
#define DEC_AIMEE_IR_METRICS_H 1

#include <aimee/ir/aimee_ir.h> /* aimee_wire_t */

typedef enum
{
   AIMEE_IR_M_RESCUE_RECOVERIES,
   AIMEE_IR_M_REASONING_OBSERVED,
   AIMEE_IR_M_REASONING_INCOMPLETE,
   AIMEE_IR_M__COUNT
} aimee_ir_metric_t;

/* Increment a counter for a given frontend wire (UNKNOWN aggregates protocol-less). */
void aimee_ir_metric_inc(aimee_ir_metric_t m, aimee_wire_t frontend);
/* Read a counter (a specific wire, or pass AIMEE_WIRE_UNKNOWN's slot). */
long aimee_ir_metric_get(aimee_ir_metric_t m, aimee_wire_t frontend);
/* Sum across all wires for a metric. */
long aimee_ir_metric_total(aimee_ir_metric_t m);
/* Stable metric name (for a /metrics dump). */
const char *aimee_ir_metric_name(aimee_ir_metric_t m);
/* Reset all counters (tests only). */
void aimee_ir_metrics_reset(void);

#endif /* DEC_AIMEE_IR_METRICS_H */
