#ifndef AIMEE_ENSEMBLE_REQUEST_H
#define AIMEE_ENSEMBLE_REQUEST_H
#include <aimee/ir/aimee_ir.h>
/* Generated synthesis instructions extend an owned task, preserving its blocks. */
int ensemble_synthesis_request(const aimee_request_t *task, const char *instructions,
                               aimee_request_t *out);
/* Append a separately labelled canonical model observation; source stays borrowed. */
int ensemble_request_add_response(aimee_request_t *request, const char *label,
                                  const aimee_response_t *response);
#endif
