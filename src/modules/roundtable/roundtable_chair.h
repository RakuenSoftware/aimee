#ifndef DEC_ROUNDTABLE_CHAIR_H
#define DEC_ROUNDTABLE_CHAIR_H

/* roundtable_chair: the reasoning-refutation pass (PR-B).
 *
 * The deterministic replay verifier (roundtable_verify.c) refutes findings the CODE
 * INDEX can contradict. But a finding can be technically true yet over-flagged — low
 * impact, or defended elsewhere in the code — which the index cannot adjudicate; it
 * takes judgment. That judgment is the CHAIR's job, not the primary agent's.
 *
 * The chair receives only the findings that SURVIVED verification, and may do exactly
 * two things, always with a written rationale: DEMOTE an over-severe finding, or DROP
 * an over-flagged one. The server ENFORCES these bounds (see roundtable_chair_apply):
 * the chair can never escalate a severity and never invent a finding — so a fallible
 * chair can only ever make the result MORE conservative, and every change it makes is
 * shown to the primary (a dropped finding moves to the rejected list with the chair's
 * rationale). This bounds the risk of adding a second fallible model to the very step
 * meant to curb fallible over-flagging. */

#include "roundtable_types.h" /* roundtable_result_t */

/* Build the chair adjudication prompt from the surviving items in `out` (an items
 * digest + the demote/drop-only contract). Returns a malloc'd string (caller frees),
 * or NULL if there is nothing to adjudicate (no items) or on allocation failure. */
char *roundtable_chair_build_prompt(const roundtable_result_t *out);

/* Apply the chair's verdict JSON to `out`, CONSERVATIVELY: match each verdict to a
 * surviving item by summary; DEMOTE only lowers severity (an escalation is ignored),
 * DROP moves the item to out->rejected[] (reason "chair-refuted"), KEEP/unknown leaves
 * it untouched. Verdicts that match no item are ignored (the chair cannot add a
 * finding). `*changed` (may be NULL) receives the count of items demoted+dropped.
 *
 * Returns a malloc'd "## Chair adjudication" markdown block listing every demote/drop
 * with the chair's rationale (caller appends it to the artifact and frees it), or NULL
 * if the chair changed nothing OR the JSON did not parse — in which case `out` is left
 * exactly as it was (the chair is best-effort and never destroys the verified result). */
char *roundtable_chair_apply(roundtable_result_t *out, const char *chair_json, int *changed);

#endif /* DEC_ROUNDTABLE_CHAIR_H */
