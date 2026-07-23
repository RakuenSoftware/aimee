#ifndef AIMEE_KB_MGMT_TOKEN_AUTHORITY_H
#define AIMEE_KB_MGMT_TOKEN_AUTHORITY_H

#include "kb_mgmt_token.h"
#include "kb_mgmt_token_roots_provision.h"
#include "org_vault_key_use.h"

#include <stddef.h>
#include <stdint.h>

#define KB_MGMT_TOKEN_AUTHORITY_CANDIDATE_MAX 128U

typedef enum
{
   KB_MGMT_TOKEN_AUTHORITY_OK = 0,
   KB_MGMT_TOKEN_AUTHORITY_INVALID,
   KB_MGMT_TOKEN_AUTHORITY_KEY_MISMATCH,
   KB_MGMT_TOKEN_AUTHORITY_CRYPTO_UNAVAILABLE,
   KB_MGMT_TOKEN_AUTHORITY_OUTPUT_TOO_SMALL
} kb_mgmt_token_authority_result_t;

/* Complete immutable tuple returned by the authority-only database facade.
 * Character arrays are canonical NUL-terminated fixed records with a zero
 * unused tail. The ordinary kb process must never receive this record. */
typedef struct
{
   int newly_admitted;
   char correlation_id[65];
   char jti[65];
   int64_t team_id;
   char actor_identity[577];
   kb_mgmt_token_capability_t capability;
   char target_server_id[128];
   char request_sha256[65];
   char token_issuer[256];
   char audience[128];
   char kid[65];
   int64_t issued_at;
   int64_t expires_at;
   char installation_id[33];
   int64_t installation_generation;
   int64_t installation_enrollment_id;
   char local_cert_issuer[512];
   char local_cert_serial_norm[80];
   char local_cert_fingerprint[65];
   int64_t target_enrollment_id;
   char target_mgmt_issuer[512];
   char target_mgmt_serial_norm[80];
   char target_mgmt_fingerprint[65];
   int64_t revocation_generation;
   int64_t publication_generation;
   char publication_candidate_id[KB_MGMT_TOKEN_AUTHORITY_CANDIDATE_MAX + 1];
   uint8_t publication_manifest_sha256[32];
   uint8_t publication_envelope_sha256[32];
   char token_custody_key_id[KB_MGMT_ROOT_CUSTODY_ID_MAX + 1];
   int64_t token_version;
   uint8_t token_public_key[KB_MGMT_TOKEN_MODULUS_LEN];
   uint8_t token_public_exponent[3];
   uint8_t token_public_digest[32];
   uint8_t token_jwk_digest[32];
   int64_t vault_seal_epoch;
   uint8_t hwm_attestation[KB_MGMT_ROOT_ATTEST_MAX];
   size_t hwm_attestation_len;
   uint8_t hwm_attestation_digest[32];
   db2_vault_key_use_envelope_t envelope;
   int64_t key_use_created_at_epoch;
} kb_mgmt_token_authority_record_t;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Authority-binary-only protected callback core. It accepts no arbitrary
    * claims or signing input: every claim and public binding comes from the
    * admitted record. PKCS#8 must contain exactly one RSA-3072/e=65537 key. */
   kb_mgmt_token_authority_result_t
   kb_mgmt_token_authority_sign_pkcs8(const kb_mgmt_token_authority_record_t *admitted,
                                      const unsigned char *pkcs8, size_t pkcs8_len, char *jwt_out,
                                      size_t jwt_cap, size_t *jwt_len);

   /* Validate the fixed record independently of private-key use. */
   int kb_mgmt_token_authority_record_valid(const kb_mgmt_token_authority_record_t *record);

#ifdef __cplusplus
}
#endif

#endif
