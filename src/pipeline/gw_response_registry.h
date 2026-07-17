/* gw_response_registry.h -- the RESPONSE-STAGE seam: a config-driven, explicit ordered
 * catalog of response transforms over the parsed reply, mirroring the request-side
 * gw_stage_registry (Slice 7) but for the response direction. Slice 1 of the
 * response/orchestration-stages proposal. Per the roundtable ruling: the stage takes a
 * TYPED context (not a raw void*) and returns a STRUCTURED result (ok/reject/error) so the
 * caller can fail-closed at runtime (never emit a reply after a governance reject/error).
 * First consumer (Slice 2): the response governance stage that wraps
 * gateway_policy_police_parsed_response, collapsing the 4 inline call sites. */
#ifndef DEC_GW_RESPONSE_REGISTRY_H
#define DEC_GW_RESPONSE_REGISTRY_H 1

#include <stddef.h>

struct parsed_response;

/* Shared context handed to every response stage. Typed (not void*) so the durable ABI is
 * not coupled to undocumented casting; grows by adding fields, never a bare pointer. */
typedef struct
{
   struct parsed_response *resp; /* the parsed reply; stages mutate in place */
} gw_response_ctx_t;

/* A stage's outcome. `ok` proceeds; `reject`/`error` stop the pipeline and MUST prevent the
 * caller from emitting a reply (fail-closed at runtime). */
typedef enum
{
   GW_RSTAGE_OK = 0,
   GW_RSTAGE_REJECT,
   GW_RSTAGE_ERROR
} gw_response_stage_status_t;

typedef struct
{
   gw_response_stage_status_t status;
   int interventions; /* audit count for OK outcomes (>=0) */
} gw_response_stage_result_t;

/* A response stage: inspect/alter ctx->resp in place, return a structured result. */
typedef gw_response_stage_result_t (*gw_response_stage_fn)(gw_response_ctx_t *ctx, void *ud);

typedef struct
{
   gw_response_stage_fn fn;
   void *ud;
   const char *name;
} gw_response_stage_t;

/* One candidate stage in an ingress's ordered catalog; enabled==0 removes the module. */
typedef struct
{
   const char *name;
   gw_response_stage_fn fn;
   void *ud;
   int enabled;
} gw_response_stage_slot_t;

/* Build the enabled, ordered stage array from `slots` into `out` (cap). Returns the count
 * (>=0), or -1 on a hard error: duplicate enabled name, enabled slot with empty name / NULL
 * fn, or output overflow. Same fail-closed contract as gw_stage_registry_build. */
int gw_response_registry_build(const gw_response_stage_slot_t *slots, size_t n_slots,
                               gw_response_stage_t *out, size_t cap);

/* Run `stages` over `ctx` in order. Sums OK interventions; STOPS at the first non-OK stage
 * and returns that status (the caller must then NOT emit a reply). A NULL/empty stage list
 * is a clean OK. */
gw_response_stage_result_t gw_response_pipeline_run(gw_response_ctx_t *ctx,
                                                    const gw_response_stage_t *stages, size_t n);

#endif /* DEC_GW_RESPONSE_REGISTRY_H */
