#ifndef AIMEE_ENSEMBLE_REQUEST_H
#define AIMEE_ENSEMBLE_REQUEST_H
#include <aimee/ir/aimee_ir.h>
/* Generated synthesis instructions extend an owned task, preserving its blocks. */
int ensemble_synthesis_request(const aimee_request_t *task, const char *instructions,
                               aimee_request_t *out);
#endif
