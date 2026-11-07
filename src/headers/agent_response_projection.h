/* Host decisions over admitted IR; original observations remain immutable. */
#ifndef AIMEE_AGENT_RESPONSE_PROJECTION_H
#define AIMEE_AGENT_RESPONSE_PROJECTION_H
#include <aimee/ir/aimee_ir.h>
#include <stddef.h>
/* -1 means persistence/allocation failed; no tool may execute after it. */
int agent_response_govern(aimee_response_t *response, int enabled, int policy, char *first_denied,
                          size_t capacity);
int agent_response_consume_final(aimee_response_t *response);
#endif
