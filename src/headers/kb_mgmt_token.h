#ifndef AIMEE_KB_MGMT_TOKEN_H
#define AIMEE_KB_MGMT_TOKEN_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Mint a short-lived KB→server management JWT.  All identity and binding
 * claims are explicit; callers must supply a unique jti and a bounded TTL. */
char *kb_mgmt_token_mint(const char *private_key_pem, const char *kid, const char *issuer,
                         const char *audience, const char *subject, const char *capability,
                         const char *cert_cn, const char *jti, long iat, long ttl_secs);
#ifdef __cplusplus
}
#endif
#endif
