/* Request field-coverage and explicit comparison diagnostics. Production
 * responses always enter canonical IR; there is no legacy response shadow. */
#ifndef DEC_AIMEE_IR_SHADOW_H
#define DEC_AIMEE_IR_SHADOW_H 1

#include <aimee/ir/aimee_ir.h> /* aimee_wire_t */

struct cJSON;

/* Observe one inbound request in shadow mode: no-op unless AIMEE_IR_SHADOW is set.
 * Parses `req` for the given frontend wire, rebuilds it, updates the ir_* metrics,
 * and logs the first few mismatches. Safe on the hot path (void, fail-silent). */
void aimee_ir_shadow_observe_request(const struct cJSON *req, aimee_wire_t frontend);

#endif /* DEC_AIMEE_IR_SHADOW_H */
