#ifndef AIMEE_KB_MGMT_TOKEN_AUTHORITY_H
#define AIMEE_KB_MGMT_TOKEN_AUTHORITY_H

#include "kb_identity_token.h"
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
typedef struct kb_mgmt_token_authority_record
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

/* Ceiling on the minted data-plane identity token's lifetime. It matches the
 * server verifier's SERVER_IDENTITY_TOKEN_MAX_LIFETIME: the authority must never
 * mint a token the server would reject outright. */
#define KB_IDENTITY_TOKEN_AUTHORITY_MAX_LIFETIME 3600

/* The identity-token counterpart of the record above (proposal
 * per-user-remote-writes-authz.md §4). Same custody, publication and HWM
 * bindings — it is the same vault-custodied RSA-3072 authority key, published
 * in the same JWKS — but the data-plane claim set instead of the management
 * one: a `tier` rather than a capability, and NO peer-cert binding and NO
 * request digest, because the bearer is a browser or thin client rather than an
 * enrolled server. `audience` is the enrolled server the token is minted for;
 * `target_enrollment_id` binds that audience to its enrollment row. */
typedef struct kb_identity_token_authority_record
{
   int newly_admitted;
   char correlation_id[65];
   char jti[129];
   int64_t team_id;
   char subject[577];
   kb_identity_tier_t tier;
   char token_issuer[256];
   char audience[128];
   char kid[65];
   int64_t issued_at;
   int64_t expires_at;
   char installation_id[33];
   int64_t installation_generation;
   int64_t installation_enrollment_id;
   int64_t target_enrollment_id;
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
} kb_identity_token_authority_record_t;

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

   /* The identity-token mint. Same contract as the management one: no arbitrary
    * claims, no caller-supplied signing input — every claim comes from the
    * admitted record, and the same PKCS#8 admission applies. */
   kb_mgmt_token_authority_result_t
   kb_identity_token_authority_sign_pkcs8(const kb_identity_token_authority_record_t *admitted,
                                          const unsigned char *pkcs8, size_t pkcs8_len,
                                          char *jwt_out, size_t jwt_cap, size_t *jwt_len);

   /* Validate the fixed identity record independently of private-key use. */
   /* 1 if `subject` is a well-formed DATA-PLANE subject. Exposed only so the grammar
    * can be cross-checked against its three other copies (tests/subject_corpus.h);
    * the authority itself reaches it through record_valid. Not the management actor
    * grammar, which is stricter and excludes the bare form. */
   int kb_identity_token_authority_subject_valid(const char *subject);

   int kb_identity_token_authority_record_valid(const kb_identity_token_authority_record_t *record);

#ifdef __cplusplus
}
#endif

#endif
