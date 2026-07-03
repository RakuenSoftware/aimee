/* router_advise.h -- S1 advisory router hook.
 *
 * Runs the request->workflow router over one turn and LOGS the routed workflow to
 * the interaction-event sink. ADVISORY ONLY: it never binds a session to a
 * workflow, never enforces, and never affects the turn (S2 does the binding).
 * Fail-safe: any error is swallowed.
 *
 * The router lives at the UNIFIED gateway seam (gw_stage_router): every inbound
 * provider request -- the primary CLI over /v1/messages AND every delegate over
 * /v1/chat/completions -- passes through it, co-located with the tool-policing
 * stage. router_advise_turn is the legacy per-message entry (still carries the
 * sampled LLM classifier). */
#ifndef DEC_ROUTER_ADVISE_H
#define DEC_ROUTER_ADVISE_H 1

#include "gateway_pipeline.h" /* gw_request_t (anonymous typedef; can't fwd-declare) */

/* Gateway pipeline stage. Extracts this turn's user query from the request and
 * records the advisory routing/enforce decision. Returns 0 (never mutates the
 * request); safe on every turn (no LLM, no recursion). */
int gw_stage_router(gw_request_t *r, void *ud);

/* Legacy per-message entry: classify `message` for `session_id`, record the
 * advisory decision, and (on a sampled DEFER) run the LLM classifier telemetry.
 * No-op on empty args / invalid catalog. */
void router_advise_turn(const char *session_id, const char *message);

/* S4 autonomous parity: pick the workflow for an autonomous submission whose
 * `workflow` was omitted. Routes `text` through the request->workflow router (no
 * LLM: intake stays deterministic and `text` is untrusted), clamped to the
 * full-spine autonomous set (floor = managed-change). Always yields a usable
 * full-spine workflow -- on a catalog fault it falls to the floor. Fills:
 *   out_wf   (>=1)  the chosen workflow id;
 *   out_src  (opt)  decision source: "prefilter"/"classifier"/"default"/"cat-error";
 *   out_raw  (opt)  the pre-clamp router id (for audit);
 *   out_tag  (opt, >=9) 8-hex FNV correlation tag of `text` (computed here so it
 *                   can be logged after `text`'s buffer is freed);
 *   *out_clamped (opt) 1 if the floor was substituted. */
void router_autonomous_pick(const char *text, char *out_wf, size_t wf_n, char *out_src,
                            size_t src_n, char *out_raw, size_t raw_n, char *out_tag, size_t tag_n,
                            int *out_clamped);

/* Append the route-s4 audit event for a committed autonomous run. Typed,
 * closed-set fields only (chosen id, source, pre-clamp id, clamp bool, FNV tag) --
 * never the proposal content, so an attacker-shaped proposal cannot poison the
 * row. Best-effort (a logging fault does not fail the submit). */
void router_autonomous_audit(const char *work_item_id, const char *chosen, const char *src,
                             const char *raw, int clamped, const char *tag);

#endif /* DEC_ROUTER_ADVISE_H */
