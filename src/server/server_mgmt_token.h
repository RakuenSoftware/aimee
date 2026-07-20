#ifndef AIMEE_SERVER_MGMT_TOKEN_H
#define AIMEE_SERVER_MGMT_TOKEN_H
#include "kb_verifier.h"
int server_mgmt_token_verify(const char *jwt, const char *jwks_json, const char *issuer,
                             const char *audience, long now, kb_verify_result_t *out);
int server_mgmt_token_verify_bound(const char *jwt, const char *jwks_json, const char *issuer,
                                   const char *audience, const char *peer_cert_cn, long now,
                                   kb_verify_result_t *out, char *capability, size_t cap_n,
                                   char *jti, size_t jti_n);
int server_mgmt_action_authorize(const char *jwt, const char *jwks_json, const char *issuer,
                                 const char *audience, const char *peer_cert_cn, long now,
                                 int remote_writes, const char *required_capability,
                                 kb_verify_result_t *out, char *jti, size_t jti_n);
#endif
