/* agent_request_shaping.h: provider-specific request shaping helpers */
#ifndef DEC_AGENT_REQUEST_SHAPING_H
#define DEC_AGENT_REQUEST_SHAPING_H 1

#include "aimee.h"
#include "agent_types.h"

struct cJSON;

int agent_request_prefers_no_think_prompt(const agent_t *agent);
char *agent_request_shape_user_prompt(const agent_t *agent, const char *user_prompt);

#endif /* DEC_AGENT_REQUEST_SHAPING_H */
