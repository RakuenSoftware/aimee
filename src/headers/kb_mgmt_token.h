#ifndef AIMEE_KB_MGMT_TOKEN_H
#define AIMEE_KB_MGMT_TOKEN_H

#include <stddef.h>
#include <stdint.h>

#define KB_MGMT_TOKEN_WIRE_MAX 8192u

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      KB_MGMT_TOKEN_CAP_REMOTE_WRITES = 1,
      KB_MGMT_TOKEN_CAP_REMOTE_READS = 2
   } kb_mgmt_token_capability_t;

   typedef struct
   {
      char issuer[256];
      char audience[128];
      char subject[577];
      int64_t team_id;
      kb_mgmt_token_capability_t capability;
      char jti[129];
      char correlation_id[129];
      char request_sha256[65];
      char peer_issuer[512];
      char peer_serial[80];
      char peer_fingerprint[65];
      char kid[65];
      int64_t issued_at;
      int64_t expires_at;
   } kb_mgmt_token_claims_t;

   typedef int (*kb_mgmt_token_sign_fn)(void *ctx, const unsigned char *signing_input,
                                        size_t signing_input_len, unsigned char *signature,
                                        size_t signature_cap, size_t *signature_len);

   typedef enum
   {
      KB_MGMT_TOKEN_OK = 0,
      KB_MGMT_TOKEN_INVALID,
      KB_MGMT_TOKEN_SIGN_UNAVAILABLE,
      KB_MGMT_TOKEN_OUTPUT_TOO_SMALL
   } kb_mgmt_token_result_t;

   /* Build the exact P5-C1 management JWT from an already-authorized immutable
    * tuple. This function never reads a clock, creates an identifier, or handles
    * private key material. The signer is called once, after complete validation
    * and worst-case output preflight. */
   kb_mgmt_token_result_t kb_mgmt_token_build(const kb_mgmt_token_claims_t *claims,
                                              kb_mgmt_token_sign_fn signer, void *signer_ctx,
                                              char *jwt_out, size_t jwt_cap, size_t *jwt_len);

#ifdef __cplusplus
}
#endif
#endif
