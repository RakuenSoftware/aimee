/* kb_curator_custom.h: composed (user-defined) curator stages — Phase D of the
 * user-configurable curator pipeline. A custom stage RECOMPOSES a vetted built-in
 * op: it reuses that op's run() function pointer under a new name/budget, appended
 * to the built-in registry the lane workers iterate. There is NO new executable
 * code — base_op must name an existing built-in stage (rejecting unknown names is
 * the injection guard).
 *
 * A custom stage runs on its base op's NATIVE lane; re-laning to a different lane
 * is deliberately disallowed in v1. Every built-in op drains its own hardcoded
 * queue via a non-atomic SELECT-then-commit; two consumers of one queue on two
 * concurrent lane threads would double-drain it (double LLM cost, racy commits).
 * Keeping a custom stage on its base op's lane makes built-in ⊕ custom sequential
 * on a single thread — safe by construction. Re-laning is a future phase gated on
 * an atomic transactional dequeue. See
 * docs/proposals/done/user-configurable-curator-pipeline.md §5.
 *
 * This module is deliberately decoupled from the drain's private CURATOR_STAGES
 * table (via an injected resolver) and dependency-light, so it is unit-testable
 * with a mock registry — mirroring kb_curator_pipeline.c. */
#ifndef DEC_KB_CURATOR_CUSTOM_H
#define DEC_KB_CURATOR_CUSTOM_H 1

#include <stddef.h>

#include "kb_curator_pipeline.h" /* kb_curator_stage_desc_t */

#define KB_CURATOR_CUSTOM_NAME_MAX   64    /* incl. NUL; names are [A-Za-z0-9_-]{1,63} */
#define KB_CURATOR_MAX_CUSTOM        32    /* cap on composed stages (DoS backstop) */
#define KB_CURATOR_CUSTOM_JSON_MAX   8192  /* refuse to parse a larger config blob */
#define KB_CURATOR_CUSTOM_BUDGET_MAX 65536 /* clamp ceiling for a per-pass budget */

/* Map a built-in stage name to its registry descriptor, or NULL if the name is
 * not a vetted built-in run()-backed stage. Injected so this module need not see
 * the drain's static CURATOR_STAGES table. */
typedef const kb_curator_stage_desc_t *(*kb_curator_base_resolver)(const char *name);

/* One validated composed stage. Owns its name storage so a derived stage
 * descriptor's const char* fields can outlive the parsed JSON. */
typedef struct
{
   char name[KB_CURATOR_CUSTOM_NAME_MAX];
   char base_op[KB_CURATOR_CUSTOM_NAME_MAX];
   const kb_curator_stage_desc_t *base; /* resolved base op (run/lane source) */
   int budget;                          /* clamped >= 1 */
   int enabled;                         /* 0/1; disabled ones are surfaced, not run */
} kb_curator_custom_t;

/* Parse operator-supplied custom_stages JSON into validated composed stages.
 * Accepts an entry when: name matches [A-Za-z0-9_-]{1,63} and collides with
 * neither a built-in (resolver != NULL) nor an earlier custom; base_op resolves to
 * a built-in; an optional "lane" equals the base op's native lane (a differing
 * lane is rejected — re-laning is not supported in v1); budget defaults to the
 * base op's budget, clamped >= 1; enabled defaults true and, if present, must be a
 * JSON bool. Unknown fields are ignored (forward-compat). Malformed/oversized JSON
 * or a bad entry is skipped without aborting the rest. Fills out[0..N)
 * (N <= max, capped at KB_CURATOR_MAX_CUSTOM); returns N. `*nrej`, if non-NULL,
 * receives the count of rejected entries so the caller can WARN once. */
size_t kb_curator_custom_stages_parse(const char *json, kb_curator_base_resolver resolve,
                                      kb_curator_custom_t *out, size_t max, int *nrej);

/* Fill a runnable stage descriptor from a parsed custom stage: reuses the base
 * op's run fn, on the base op's lane, with the custom name/budget and
 * enabled=NULL (presence == enabled; disabled customs are simply not composed).
 * `c` must outlive `desc` — desc->name and desc->label point into c->name. */
void kb_curator_custom_to_desc(const kb_curator_custom_t *c, kb_curator_stage_desc_t *desc);

#endif /* DEC_KB_CURATOR_CUSTOM_H */
