#ifndef AIMEE_KB_MGMT_JWKS_PUBLICATION_H
#define AIMEE_KB_MGMT_JWKS_PUBLICATION_H

#include "kb_mgmt_token_roots_provision.h"

#include <stddef.h>
#include <stdint.h>

#define KB_MGMT_JWKS_GENERATION       1
#define KB_MGMT_JWKS_SIGNATURE_LEN    64
#define KB_MGMT_JWKS_SHA256_LEN       32
#define KB_MGMT_JWKS_CANDIDATE_ID_LEN 64
#define KB_MGMT_JWKS_BYTES_MAX        1024
#define KB_MGMT_JWKS_PAYLOAD_MAX      2048
#define KB_MGMT_JWKS_ENVELOPE_MAX     3072
#define KB_MGMT_JWKS_TIME_MAX         INT64_C(9007199254740991)

typedef enum
{
   KB_MGMT_JWKS_EMPTY = 0,
   KB_MGMT_JWKS_STAGED = 1,
   KB_MGMT_JWKS_CAS_DONE = 2,
   KB_MGMT_JWKS_FINAL = 3,
} kb_mgmt_jwks_phase_t;

typedef enum
{
   KB_MGMT_JWKS_FRESH = 0,
   KB_MGMT_JWKS_RECOVERED = 1,
   KB_MGMT_JWKS_CONVERGED = 2,
   KB_MGMT_JWKS_RETRY = 3,
   KB_MGMT_JWKS_SEALED = 4,
   KB_MGMT_JWKS_CONFLICT = 5,
   KB_MGMT_JWKS_INTEGRITY = 6,
} kb_mgmt_jwks_result_t;

typedef enum
{
   KB_MGMT_JWKS_DB_OK = 0,
   KB_MGMT_JWKS_DB_RETRY = 1,
   KB_MGMT_JWKS_DB_SEALED = 2,
   KB_MGMT_JWKS_DB_CONFLICT = 3,
   KB_MGMT_JWKS_DB_INTEGRITY = 4,
} kb_mgmt_jwks_db_result_t;

typedef enum
{
   KB_MGMT_JWKS_HWM_OK = 0,
   KB_MGMT_JWKS_HWM_COMPARE = 1,
   KB_MGMT_JWKS_HWM_RETRY = 2,
   KB_MGMT_JWKS_HWM_INTEGRITY = 3,
} kb_mgmt_jwks_hwm_result_t;

typedef struct
{
   kb_mgmt_root_record_t token;
   kb_mgmt_root_record_t manifest;
   kb_mgmt_publication_root_t publication;
} kb_mgmt_jwks_roots_t;

typedef struct
{
   kb_mgmt_jwks_phase_t phase;
   uint64_t generation;
   int64_t valid_from;
   int64_t valid_until;
   char candidate_id[KB_MGMT_JWKS_CANDIDATE_ID_LEN + 1];
   char manifest_id[KB_MGMT_MANIFEST_ID_MAX + 1];
   uint8_t token_public_digest[32];
   uint8_t token_jwk_digest[32];
   uint8_t manifest_public_digest[32];
   uint8_t publication_identity_digest[32];
   uint64_t seal_epoch;
   char jwks[KB_MGMT_JWKS_BYTES_MAX];
   size_t jwks_len;
   uint8_t jwks_digest[32];
   char payload[KB_MGMT_JWKS_PAYLOAD_MAX];
   size_t payload_len;
   uint8_t payload_digest[32];
   uint8_t signature[KB_MGMT_JWKS_SIGNATURE_LEN];
   char envelope[KB_MGMT_JWKS_ENVELOPE_MAX];
   size_t envelope_len;
   uint8_t manifest_digest[32];
   uint8_t envelope_digest[32];
   uint8_t hwm1_attestation[KB_MGMT_ROOT_ATTEST_MAX];
   size_t hwm1_attestation_len;
   uint8_t hwm2_attestation_digest[KB_MGMT_JWKS_SHA256_LEN];
} kb_mgmt_jwks_record_t;

typedef struct
{
   int64_t valid_from;
   int64_t valid_until;
   int64_t now;
   uint64_t clock_skew_seconds;
   uint64_t maximum_lifetime_seconds;
} kb_mgmt_jwks_config_t;

typedef struct
{
   kb_mgmt_jwks_db_result_t (*inspect)(void *, kb_mgmt_jwks_roots_t *, kb_mgmt_jwks_record_t *);
   kb_mgmt_jwks_db_result_t (*stage)(void *, const kb_mgmt_jwks_record_t *);
   kb_mgmt_jwks_db_result_t (*record_cas)(void *, const kb_mgmt_jwks_record_t *, const uint8_t *,
                                          size_t);
   kb_mgmt_jwks_db_result_t (*finalize)(void *, const kb_mgmt_jwks_record_t *);
   kb_mgmt_jwks_hwm_result_t (*hwm_read)(void *, const char *, uint64_t *, uint8_t *, size_t,
                                         size_t *);
   kb_mgmt_jwks_hwm_result_t (*hwm_cas)(void *, const char *, uint64_t, uint64_t, uint8_t *, size_t,
                                        size_t *);
   int (*hwm_verify)(void *, const char *, uint64_t, const uint8_t *, size_t);
   kb_mgmt_jwks_result_t (*protected_sign)(void *, const kb_mgmt_root_record_t *, uint64_t,
                                           const char *, const uint8_t[32], const uint8_t *, size_t,
                                           uint8_t[KB_MGMT_JWKS_SIGNATURE_LEN]);
   void *ctx;
} kb_mgmt_jwks_callbacks_t;

int kb_mgmt_jwks_build_unsigned(const uint8_t *token_modulus, size_t token_modulus_len,
                                int64_t valid_from, int64_t valid_until,
                                kb_mgmt_jwks_record_t *record);
int kb_mgmt_jwks_complete(const uint8_t manifest_public[32], const char *manifest_id,
                          const uint8_t signature[KB_MGMT_JWKS_SIGNATURE_LEN],
                          kb_mgmt_jwks_record_t *record);
int kb_mgmt_jwks_validate(const kb_mgmt_jwks_roots_t *, const kb_mgmt_jwks_record_t *);
int kb_mgmt_jwks_ed25519_sign(const uint8_t seed[32], const uint8_t *payload, size_t payload_len,
                              uint8_t signature[KB_MGMT_JWKS_SIGNATURE_LEN]);
int kb_mgmt_jwks_ed25519_verify(const uint8_t public_key[32], const uint8_t *payload,
                                size_t payload_len,
                                const uint8_t signature[KB_MGMT_JWKS_SIGNATURE_LEN]);

kb_mgmt_jwks_result_t kb_mgmt_jwks_publish(const kb_mgmt_jwks_config_t *,
                                           const kb_mgmt_jwks_callbacks_t *, char *, size_t,
                                           size_t *);
kb_mgmt_jwks_result_t kb_mgmt_jwks_export(const kb_mgmt_jwks_callbacks_t *, char *, size_t,
                                          size_t *);

#endif
