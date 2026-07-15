/* aimee_ir_metrics.h -- shadow-mode observability for the canonical-IR refactor.
 * The rewrite ships behind a config-only flag with the old path as fallback; these
 * counters let us detect adapter failures + parity drift in SHADOW mode (both paths
 * run, results compared) BEFORE flipping any traffic to the IR path. A nonzero
 * rebuild-mismatch or cache-control-lost count is a BUG, not noise. Thread-safe
 * (the ingress is multi-threaded): counters are atomic. */
#ifndef DEC_AIMEE_IR_METRICS_H
#define DEC_AIMEE_IR_METRICS_H 1

#include "aimee_ir.h" /* aimee_wire_t */

typedef enum
{
   AIMEE_IR_M_PARSE_FAIL = 0,     /* frontend.parse(client req) failed */
   AIMEE_IR_M_RENDER_FAIL,        /* frontend.render(IR) failed */
   AIMEE_IR_M_BACKEND_BUILD_FAIL, /* backend.build(IR) failed */
   AIMEE_IR_M_BACKEND_PARSE_FAIL, /* backend.parse(provider resp) failed */
   AIMEE_IR_M_REBUILD_MISMATCH,   /* same-protocol round-trip != original bytes (BUG) */
   AIMEE_IR_M_STAGE_MUTATION,     /* a core stage mutated the request (forces IR path) */
   AIMEE_IR_M_CACHE_CONTROL_LOST, /* a cache_control marker was dropped in round-trip (BUG) */
   AIMEE_IR_M_PASSTHROUGH,        /* same-protocol raw-passthrough fast-path taken */
   AIMEE_IR_M_IR_PATH,            /* full parse->IR->build path taken */
   /* The IR build returned NULL and the caller USED the legacy translator. This is
    * the rollout gate: the legacy translators cannot be deleted until this reads 0
    * over a real observation window. The *_FAIL counters say a stage failed;
    * this says a request was actually SERVED BY LEGACY. */
   AIMEE_IR_M_LEGACY_FALLBACK,
   /* Shadow: the IR-built provider body differed from what the legacy translator
    * would have sent for the SAME request. This is the direct evidence for
    * retiring the translators: 0 mismatches over real traffic means the IR is a
    * faithful replacement, not merely "it worked". */
   AIMEE_IR_M_BODY_MISMATCH,
   /* Shadow: IR and legacy produced byte-identical provider bodies. */
   AIMEE_IR_M_BODY_MATCH,
   AIMEE_IR_M_RESCUE_RECOVERIES,
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
