/* Canonical completion-quality response intervention. */
#ifndef DEC_GW_STAGE_COMPLETION_H
#define DEC_GW_STAGE_COMPLETION_H 1
#include <aimee/ir/aimee_ir.h>

/* An active repair with an available caller-owned shell and no prior bounded
 * completion attempt must buffer the response before applying this stage. */
int gw_ir_response_completion_armed(const aimee_request_t *request);
/* Preserve original content and append an inert shell call when a final answer
 * confirms matching defects but defers their repair. Returns 1 on intervention,
 * 0 when inapplicable, -1 on allocation failure. */
int gw_ir_response_run_completion(aimee_response_t *response, const aimee_request_t *request);
/* Allocated provider instructions, or NULL when inapplicable. */
char *gw_ir_request_tool_system_prompt(const aimee_request_t *request, const char *base);
char *gw_ir_request_completion_system_prompt(const aimee_request_t *request, const char *base);

#define AIMEE_COMPLETION_CALL_PREFIX "call_aimee_completion_"
#endif
