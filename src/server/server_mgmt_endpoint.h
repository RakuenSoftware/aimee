#ifndef AIMEE_SERVER_MGMT_ENDPOINT_H
#define AIMEE_SERVER_MGMT_ENDPOINT_H
#include <stddef.h>
#include "server_mgmt_audit.h"
int server_mgmt_endpoint_dispatch(const char *jwt, const char *jwks, const char *issuer,
                                  const char *audience, const char *peer_cn,
                                  const char *required_cap, const char *target,
                                  const char *request_digest, server_mgmt_action_fn action,
                                  void *ctx, char *actor, size_t actor_cap,
                                  char *jti, size_t jti_cap);
#endif
