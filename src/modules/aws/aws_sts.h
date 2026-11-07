/* modules/aws/aws_sts.h: AWS STS request CONSTRUCTION + response PARSE (P6a).
 *
 * Pure/offline: builds the form-encoded bodies for the two federation modes and
 * (for AssumeRole) the SigV4-signed request, verifies a web-identity JWT against
 * a caller-supplied JWKS, and parses the STS XML response. NO dispatch, NO
 * network, NO clock (now is passed in). The live HTTP call is a deferred (P6b)
 * caller concern.
 *
 * Two MODE-DISTINCT builders, never conflated:
 *   mode (b) AssumeRole              — SigV4-signed with the vault-held IAM key;
 *                                      carries ExternalId (confused-deputy defense).
 *   mode (a) AssumeRoleWithWebIdentity — UNSIGNED (the token is the credential);
 *                                      NO ExternalId (trust-policy aud/sub enforced
 *                                      AWS-side).
 * Both set DurationSeconds=900 explicitly (the STS floor == our TTL ceiling).
 *
 * Depends on libc + OpenSSL + cJSON (JWT claim parse). */
#ifndef DEC_AWS_STS_H
#define DEC_AWS_STS_H 1

#include "aws_sigv4.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define AWS_STS_DURATION_SECONDS 900

   /* --- form-body construction --- */

   /* The signed AssumeRole request: the form body + the SigV4 result over it. */
   typedef struct
   {
      char body[8192];
      aws_sigv4_result_t sig;
   } aws_sts_signed_request_t;

   /* --- web-identity JWT validation (verify-then-trust) --- */

   typedef enum
   {
      AWS_WEBID_OK = 0,
      AWS_WEBID_ERR_MALFORMED,       /* not a well-formed compact JWS */
      AWS_WEBID_ERR_UNSUPPORTED_ALG, /* alg not RS256/ES256 (or "none") */
      AWS_WEBID_ERR_NO_KEY,          /* no matching JWKS key */
      AWS_WEBID_ERR_BAD_SIGNATURE,   /* signature verification failed */
      AWS_WEBID_ERR_ISS,             /* iss != expected */
      AWS_WEBID_ERR_AUD,             /* aud does not contain expected */
      AWS_WEBID_ERR_EXPIRED,         /* exp <= now (with skew) */
      AWS_WEBID_ERR_IAT,             /* iat missing / future / over-age */
      AWS_WEBID_ERR_CLAIMS           /* required claim missing / malformed */
   } aws_webid_status_t;

   typedef struct
   {
      char subject[256];
      char issuer[512];
      char audience[512];
      long expiry;
      long issued_at;
   } aws_webid_claims_t;

   /* Verify a web-identity token: (1) RS256/ES256 signature against `jwks_json`,
    * THEN (2) iss==expected_iss, aud contains expected_aud, exp>now, iat sane. The
    * token is NEVER persisted. Returns AWS_WEBID_OK and fills `out` on success, else
    * a specific error code. `out` may be NULL. A wrong-key/forged token is rejected
    * BEFORE any claim is trusted (blocks cache-poisoning). */
   aws_webid_status_t aws_webidentity_validate(const char *token, const char *jwks_json,
                                               const char *expected_iss, const char *expected_aud,
                                               long now, aws_webid_claims_t *out);

   /* --- STS XML response parse (hostile-input-safe) --- */

   typedef struct
   {
      char access_key_id[256];
      char secret_access_key[256];
      char session_token[8192];
      char expiration[64];
   } aws_sts_credentials_t;

#ifdef __cplusplus
}
#endif

#endif /* DEC_AWS_STS_H */
