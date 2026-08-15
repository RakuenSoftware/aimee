#ifndef AIMEE_KB_CALLER_TOKEN_H
#define AIMEE_KB_CALLER_TOKEN_H

#include <stddef.h>
#include <stdint.h>

#include "server_identity_token.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Verify the caller token at the KB trust boundary. The token must be a
    * KB-signed aimee-id+jwt for this exact server and team, and its subject
    * must be an OIDC identity. Host callers use the separately constrained
    * server assertion path; they are never accepted through this verifier. */
   server_identity_token_result_t kb_caller_token_verify(const char *jwt, size_t jwt_len,
                                                         const char *jwks_json,
                                                         const char *server_id, int64_t named_team,
                                                         int64_t now,
                                                         server_identity_token_claims_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_KB_CALLER_TOKEN_H */
