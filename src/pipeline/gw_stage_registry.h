/* gw_stage_registry.h -- a config-driven, explicit ordered CATALOG over the
 * gw_stage_t request-stage interface (gateway_pipeline.h). Each ingress declares an
 * ORDERED list of candidate stages with a per-stage `enabled` flag (resolved from
 * config at the call site, so the stages themselves stay config-free); the builder
 * filters to the enabled ones, preserving order, and rejects duplicate names. This
 * is how a module (e.g. memory) is added/removed "at will": flip its enabled flag in
 * config and the stage is included or omitted from the pipeline. Slice 7 of the
 * canonical-IR/pluggable-stages proposal. */
#ifndef DEC_GW_STAGE_REGISTRY_H
#define DEC_GW_STAGE_REGISTRY_H 1

#include <stddef.h>

#include "gateway_pipeline.h"

/* One candidate stage in an ingress's ordered catalog. `enabled == 0` removes the
 * module from the built pipeline. `name` is a stable id used for order-independent
 * duplicate detection and for audit/trace. */
typedef struct
{
   const char *name;
   gw_request_stage_fn fn;
   void *ud;
   int enabled;
} gw_stage_slot_t;

/* Build the enabled, ordered gw_stage_t[] from `slots` (length `n_slots`) into `out`
 * (capacity `cap`). Preserves catalog order. Returns the number of enabled stages
 * written (>=0), or -1 on a hard error: a duplicate stage name among the enabled
 * set, an enabled slot with an empty name or NULL fn, or `out` too small. A hard
 * error means a misconfigured/programming catalog and must abort the request path
 * rather than silently drop stages. */
int gw_stage_registry_build(const gw_stage_slot_t *slots, size_t n_slots, gw_stage_t *out,
                            size_t cap);

#endif /* DEC_GW_STAGE_REGISTRY_H */
