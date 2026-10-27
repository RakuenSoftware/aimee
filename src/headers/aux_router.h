/* aux_router.h: auxiliary model routing for cheap per-task completions. */
#ifndef DEC_AUX_ROUTER_H
#define DEC_AUX_ROUTER_H 1

#include "config.h"
#include <aimee/ir/aimee_ir.h>

/* Route the canonical request without rebuilding its message content. Configured
 * task/model/token policy is applied to a clone. On success out owns a complete
 * canonical response; callers render text only at their external boundary. */
int aux_call(const char *task_name, const aimee_request_t *input, aimee_response_t *out);

/* Print the resolved task->provider/model mapping to stdout. */
void aux_config_show(void);

#endif /* DEC_AUX_ROUTER_H */
