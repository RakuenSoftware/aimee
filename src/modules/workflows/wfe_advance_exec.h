/* wfe_advance_exec.h -- S2 sub-slice 3: the interactive driver.
 *
 * Turns an `advance_request` tool call from a bound primary session into exactly
 * one authoritative engine step. This is the integration layer over the pure core
 * (wfe_advance.h): it resolves the caller's session->work-item binding, reads the
 * work-item's actual state, applies the pure CAS/replay decision, and -- only on a
 * clean OK -- calls wfe_engine_advance under the engine's own invariants (it never
 * mutates gate.deliver / run-state directly; the engine runs wfe_deliver on the
 * gate.deliver node). Every call is audited.
 *
 * Default-OFF: with the enforcement dial unset (WFE_ENFORCE_OFF) the driver is
 * inert. It is only live once an operator opts a session into a bound workflow. */
#ifndef DEC_WFE_ADVANCE_EXEC_H
#define DEC_WFE_ADVANCE_EXEC_H 1

#include <stddef.h>

/* Execute an `advance_request` for `session_id` with the raw tool-call arguments
 * `args_json`. Writes a JSON result object into `out`/`out_n` (always a valid,
 * NUL-terminated object when out_n > 0) describing what happened:
 *   {"status":"ok","work_item_id":..,"from_stage":..,"stage":..,"state":..,
 *    "terminal":bool}
 *   {"status":"replay"|"stale"|"unbound"|"terminal"|"badargs"|"disabled"|"error",
 *    "work_item_id":..,"actual_stage":..}   (fields present as relevant)
 *
 * The status mirrors wfe_advance_outcome_name plus "disabled" (dial off) and
 * "error" (engine/DB fault). Returns 0 when a result was produced (INCLUDING a
 * policy refusal such as stale/unbound -- those are decisions, not errors), -1
 * only on a hard fault where no result JSON could be written. */
int wfe_advance_request_run(const char *session_id, const char *args_json, char *out, size_t out_n);

#endif /* DEC_WFE_ADVANCE_EXEC_H */
