#ifndef AIMEE_KB_WORKLOAD_JWT_H
#define AIMEE_KB_WORKLOAD_JWT_H

#include "kb_workload_provider.h"

#include <stddef.h>
#include <stdint.h>

/* Verify and extract one fresh workload JWT.  token and jwks are deliberately
 * length-delimited snapshots; neither need be NUL-terminated.  The output is
 * cleared before validation and on every unsuccessful return. */
kb_workload_result_t kb_workload_jwt_validate(const void *token, size_t token_len, const void *jwks,
                                              size_t jwks_len, const char *expected_issuer,
                                              const char *expected_audience, uint64_t now,
                                              uint32_t max_token_age_seconds,
                                              kb_workload_identity_t *out);

#endif
