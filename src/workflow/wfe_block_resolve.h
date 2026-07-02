/* wfe_block_resolve.h -- S2 sub-slice 4: the per-block enforcement resolver + the
 * dispatch-time externalization guard.
 *
 * A bound interactive session's tool surface is governed by whichever workflow
 * block is currently active. This module resolves that context from AUTHORITATIVE
 * DB state (binding -> work-item -> pinned workflow -> current node) and applies
 * the pre-delivery externalization guard (Q3) at the tool-DISPATCH seam: a tool
 * call in an enforced run whose gate.deliver has not passed is refused if it is an
 * externalization primitive. Tool VISIBILITY (stripping the surface at ingress) is
 * a separate, coarser layer; this is the narrow run-delivery-state invariant that
 * a per-call check enforces even when a tool slips past visibility (consult Q2/Q3,
 * "tool visibility alone is not a security boundary").
 *
 * Default-OFF: with the enforcement dial unset the guard is inert. Design per the
 * S2 roundtable consult (2026-07-01). */
#ifndef DEC_WFE_BLOCK_RESOLVE_H
#define DEC_WFE_BLOCK_RESOLVE_H 1

#include "wfe_enforce.h" /* wfe_tool_surface_t, wfe_enforce_stage_t */

/* The per-block enforcement context for a session's current work-item. */
typedef struct
{
   int bound;                  /* 1 if the session is bound to a resolvable work-item */
   int enforced;               /* 1 if the pinned workflow is enforced (I2 => terminal
                                  is gate.deliver, so delivered==accepted is sound) */
   wfe_tool_surface_t surface; /* tool surface of the current block */
   int delivered;              /* 1 if gate.deliver has passed for this run */
   int advanceable;            /* 1 if the current block advances via advance_request
                                  (a producing block, not a gate) */
   char work_item_id[80];
   char stage[64];
} wfe_block_ctx_t;

/* Resolve the per-block context for `session_id` from authoritative DB state. On an
 * unbound session, a vanished work-item, or any load failure, zeroes `out` (bound=0)
 * and returns 0 -- the caller then applies NO per-block restriction (non-bound
 * sessions stay generic; a binding-layer failure is isolated). Returns 1 if bound +
 * resolved. */
int wfe_block_resolve(const char *session_id, wfe_block_ctx_t *out);

/* The action a bound session's tool call should take. */
typedef enum
{
   WFE_TC_ALLOW = 0, /* permitted (or dial off / unbound / non-externalization) */
   WFE_TC_WARN,      /* soft dial: a pre-delivery externalization -- warn + allow */
   WFE_TC_DENY       /* hard dial: a pre-delivery externalization -- refuse */
} wfe_toolcall_action_t;

/* PURE decision: given the dial stage and whether policy blocks this call (the
 * session is bound and the tool is a pre-delivery externalization primitive), map
 * to an action. advisory/off never restrict; soft warns; hard denies. */
wfe_toolcall_action_t wfe_toolcall_decide(wfe_enforce_stage_t stage, int policy_blocks);

/* The dispatch-time guard, composed: resolve `session_id`, and if the enforcement
 * dial is on and `tool_name` is an externalization primitive that this run has not
 * yet earned (gate.deliver not passed), return WARN/DENY per the dial; otherwise
 * ALLOW. Reads the dial from AIMEE_WORKFLOW_ENFORCE_STAGE (default OFF -> ALLOW). */
wfe_toolcall_action_t wfe_mcp_toolcall_action(const char *session_id, const char *tool_name);

#endif /* DEC_WFE_BLOCK_RESOLVE_H */
