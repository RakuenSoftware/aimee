#include "server_mgmt_token.h"
#include "kb_auth_oidc.h"
#include <stdio.h>
#include <string.h>
int server_mgmt_token_verify(const char *jwt, const char *jwks_json, const char *issuer,
                             const char *audience, long now, kb_verify_result_t *out)
{
   if (!jwt || !jwks_json || !*jwks_json || !out) return 0;
   kb_oidc_config_t c; memset(&c, 0, sizeof(c));
   if (issuer) snprintf(c.issuer, sizeof(c.issuer), "%s", issuer);
   if (audience) snprintf(c.audience, sizeof(c.audience), "%s", audience);
   snprintf(c.jwks_json, sizeof(c.jwks_json), "%s", jwks_json);
   return kb_oidc_verify_jwt(jwt, &c, now, out);
}
