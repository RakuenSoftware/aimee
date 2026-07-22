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

   /* Build the AssumeRole (mode b) form body into out[cap]:
    * Action=AssumeRole&DurationSeconds=900&ExternalId=..&Policy=..&RoleArn=..&
    * RoleSessionName=.. (values RFC3986-encoded, keys in a stable order). This body
    * is SigV4-signed by the caller (see aws_sts_build_signed_assume_role). Returns 0
    * on success, -1 on error/overflow. session_policy_json may be NULL (omit Policy);
    * external_id is REQUIRED for mode b. */
   int aws_sts_assume_role_body(char *out, size_t cap, const char *role_arn,
                                const char *role_session_name, const char *external_id,
                                const char *session_policy_json);

   /* Build the AssumeRoleWithWebIdentity (mode a) form body into out[cap]:
    * Action=AssumeRoleWithWebIdentity&DurationSeconds=900&Policy=..&RoleArn=..&
    * RoleSessionName=..&WebIdentityToken=.. — NO ExternalId. This request is UNSIGNED.
    * Returns 0 on success, -1 on error/overflow. */
   int aws_sts_assume_role_web_identity_body(char *out, size_t cap, const char *role_arn,
                                             const char *role_session_name,
                                             const char *web_identity_token,
                                             const char *session_policy_json);

   /* The signed AssumeRole request: the form body + the SigV4 result over it. */
   typedef struct
   {
      char body[8192];
      aws_sigv4_result_t sig;
   } aws_sts_signed_request_t;

   /* Build a SigV4-signed AssumeRole POST to sts.<region>.amazonaws.com using the
    * (vault-held) IAM key. Signs host + x-amz-date + content-type over the exact
    * body bytes (payload = SHA-256 of the body). `amz_date` (YYYYMMDDTHHMMSSZ) and
    * `date` (YYYYMMDD) are passed in (deterministic). Returns 0 / -1. */
   int aws_sts_build_signed_assume_role(aws_sts_signed_request_t *out, const char *region,
                                        const char *host, const char *access_key_id,
                                        const char *secret_access_key, const char *session_token,
                                        const char *role_arn, const char *role_session_name,
                                        const char *external_id, const char *session_policy_json,
                                        const char *amz_date, const char *date);

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

   /* Parse an AssumeRole / AssumeRoleWithWebIdentity XML response into `out`. Hand-
    * rolled bounded scanner: NO external-entity resolution (rejects DOCTYPE/ENTITY),
    * takes the FIRST <Credentials> block, REJECTS duplicate credential fields and a
    * trailing alternate <Credentials>/result, bounds every field. A missing
    * AccessKeyId/SecretAccessKey/SessionToken/Expiration -> error. Returns 0 / -1. */
   int aws_sts_parse_assume_response(const char *xml, aws_sts_credentials_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_AWS_STS_H */
