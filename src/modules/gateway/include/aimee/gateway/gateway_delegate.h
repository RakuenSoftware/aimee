/* Delegate request policy consumes owned IR before explicit native egress.
 * Primary turns keep their own delegation tools; the selected model and host
 * context are never rewritten here. */
#ifndef DEC_GATEWAY_DELEGATE_H
#define DEC_GATEWAY_DELEGATE_H 1

#include <aimee/gateway/gateway_policy.h>

/* Borrowed request. Returns interventions, or -1 for a missing request. */
int gateway_delegate_run_request_pipeline(aimee_request_t *request, int is_delegate);

#endif /* DEC_GATEWAY_DELEGATE_H */
