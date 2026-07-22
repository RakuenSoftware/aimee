#include "server_mgmt_endpoint.h"

int server_mgmt_endpoint_dispatch(const char *jwt, const char *jwks, const char *issuer,
                                  const char *audience, const char *peer_cn,
                                  const char *required_cap, const char *target, const char *digest,
                                  server_mgmt_action_fn action, void *ctx, char *actor,
                                  size_t actor_cap, char *jti, size_t jti_cap)
{
   (void)jwt;
   (void)jwks;
   (void)issuer;
   (void)audience;
   (void)peer_cn;
   (void)required_cap;
   (void)target;
   (void)digest;
   (void)action;
   (void)ctx;
   (void)actor;
   (void)actor_cap;
   (void)jti;
   (void)jti_cap;

   /* P5-C1a deliberately has no action surface.  Later composition must first
    * verify the B3/C2 context and durably consume the token's jti. */
   return -1;
}
