#ifndef AIMEE_SERVER_LEARNING_APPLICATION_H
#define AIMEE_SERVER_LEARNING_APPLICATION_H
#include "request_context.h"
#include "cJSON.h"
/* Exposure/scope must come from the principal-owned host receipt reader.
 * Public event fields never establish identity, exposure or human authority. */
cJSON *server_learning_application_local(const request_context_t *owner, const cJSON *event,
                                         const cJSON *exposure, const char *scope_kind,
                                         const char *scope_id);
#endif
