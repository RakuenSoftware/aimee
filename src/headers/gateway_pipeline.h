/* gateway_pipeline.h: the canonical request IR + typed stage interface for the
 * universal gateway (P2a). Every proxied model call is wrapped in a `gw_request_t`
 * and run through an ordered list of request stages — bounded, per-call transforms
 * that inspect/alter the request before it is rendered to the serving model's API.
 *
 * This is the SEAM only. The concrete stages (memory/context injection, tool
 * policing) live where their dependencies are (the ingress / the core
 * gateway_policy module); this module just defines the IR and runs the stages in
 * order. It therefore has NO dependency on server types — `driver`/`ag` are carried
 * as opaque pointers that the stage implementations cast back, and `raw` is an
 * opaque `cJSON` the stages mutate. So both ingresses AND the agentic /v1/runs path
 * can share one pipeline without a layering violation.
 *
 * Two phases, kept deliberately distinct:
 *   1. mutation stages  — gw_pipeline_run_request(): each stage rewrites `raw`.
 *   2. terminal render  — the caller, AFTER the pipeline, translates `raw` to the
 *      serving provider's message/tool shape. Render is NOT a stage (it produces a
 *      derived shape rather than mutating `raw`), so stages always see the full,
 *      un-translated request. A future need for a stage to see the post-translate
 *      shape is a response-side concern (P2b), not a P2a request stage.
 */
#ifndef DEC_GATEWAY_PIPELINE_H
#define DEC_GATEWAY_PIPELINE_H 1

#include <stddef.h>

struct cJSON;

/* The serving model's wire API — derived from the driver, never configured. */
typedef enum
{
   GW_API_ANTHROPIC = 0,
   GW_API_OPENAI = 1,
} gw_api_t;

/* Canonical request IR. `raw` is BORROWED: the caller owns the cJSON request and
 * frees it; stages mutate it in place and must never free or replace it. `driver`
 * and `ag` are opaque (the stage implementations own the concrete types). INVARIANT:
 * `parity` ⇔ the serving API equals the client's API (here: client is always
 * Anthropic /v1/messages, so `parity == (serving_api == GW_API_ANTHROPIC)`). */
typedef struct
{
   struct cJSON *raw;    /* borrowed; stages mutate in place, never free */
   const void *driver;   /* opaque delegate_driver_t* for stage impls */
   const void *ag;       /* opaque agent_t* for stage impls */
   gw_api_t serving_api; /* derived from the driver */
   int parity;           /* serving_api == client_api (no translation needed) */
   int stream;           /* this call is an SSE stream */
} gw_request_t;

/* A request stage: inspect/alter `r->raw` in place. Returns the number of
 * interventions it made (≥0, for the caller's audit/accounting) or <0 on a hard
 * error (which short-circuits the pipeline). A no-op stage (policy off, nothing to
 * do) returns 0. */
typedef int (*gw_request_stage_fn)(gw_request_t *r, void *ud);

typedef struct
{
   gw_request_stage_fn fn;
   void *ud;
   const char *name; /* for audit/trace; not interpreted by the runner */
} gw_stage_t;

/* Run `stages` (length `n`) over `r` in order. Sums each stage's intervention
 * count; returns the total (≥0), or the first stage's negative return (stopping
 * immediately — a failed transform must not let a half-altered request proceed).
 * `stages` may be NULL/`n==0` (returns 0). */
int gw_pipeline_run_request(gw_request_t *r, const gw_stage_t *stages, size_t n);

#endif /* DEC_GATEWAY_PIPELINE_H */
