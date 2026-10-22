#ifndef AIMEE_SERVER_LEARNING_APPROACHES_H
#define AIMEE_SERVER_LEARNING_APPROACHES_H
#include "request_context.h"
#include "cJSON.h"
/* Caller identity and registered project come from authenticated host owners.
 * Goal is an echo/search intention, never a claim of matching task semantics. */
cJSON *server_learning_approaches_local(const request_context_t *owner, const char *cwd,
                                        const char *goal);
#endif
