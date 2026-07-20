#ifndef AIMEE_SERVER_MGMT_TOKEN_H
#define AIMEE_SERVER_MGMT_TOKEN_H
#include "kb_verifier.h"
int server_mgmt_token_verify(const char *jwt, const char *jwks_json, const char *issuer,
                             const char *audience, long now, kb_verify_result_t *out);
#endif
