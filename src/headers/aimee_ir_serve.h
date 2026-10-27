/* Canonical native-wire ingress/build seams. Admission is mandatory;
 * disabling host context edits never selects a legacy provider builder. */
#ifndef DEC_AIMEE_IR_SERVE_H
#define DEC_AIMEE_IR_SERVE_H 1

#include <stddef.h>

#include <aimee/ir/aimee_ir.h> /* aimee_response_t */

struct cJSON;

/* Apply the configured host stages to an already admitted canonical request. */
void aimee_ir_apply_gateway_request_stages(aimee_request_t *request);

/* Native Anthropic retains its configured context-injection opt-in while using
 * the same canonical stage catalog as translated requests. */
void aimee_ir_apply_native_request_stages(aimee_request_t *request);

#endif /* DEC_AIMEE_IR_SERVE_H */
