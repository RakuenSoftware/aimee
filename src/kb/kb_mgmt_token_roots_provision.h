#ifndef AIMEE_KB_MGMT_TOKEN_ROOTS_PROVISION_H
#define AIMEE_KB_MGMT_TOKEN_ROOTS_PROVISION_H

#include "kb_mgmt_token_public.h"
#include "vault_crypto.h"

#include <stddef.h>
#include <stdint.h>

#define KB_MGMT_ROOT_CUSTODY_ID_MAX 600
#define KB_MGMT_ROOT_BOOTSTRAP_LEN  64
#define KB_MGMT_MANIFEST_ID_MAX     64
#define KB_MGMT_ROOT_ATTEST_MAX     512
#define KB_MGMT_ROOT_SECRET_MAX     4096
#define KB_MGMT_PUBLIC_BUNDLE_MAX   1536

typedef enum
{
   KB_MGMT_ROOT_TOKEN = 1,
   KB_MGMT_ROOT_MANIFEST = 2,
} kb_mgmt_root_kind_t;

typedef enum
{
   KB_MGMT_ROOT_EMPTY = 0,
   KB_MGMT_ROOT_STAGED = 1,
   KB_MGMT_ROOT_CAS_DONE = 2,
   KB_MGMT_ROOT_FINAL = 3,
} kb_mgmt_root_phase_t;

typedef enum
{
   KB_MGMT_ROOT_DB_OK = 0,
   KB_MGMT_ROOT_DB_RETRY = 1,
   KB_MGMT_ROOT_DB_SEALED = 2,
   KB_MGMT_ROOT_DB_CONFLICT = 3,
   KB_MGMT_ROOT_DB_INTEGRITY = 4,
} kb_mgmt_root_db_result_t;

typedef enum
{
   KB_MGMT_ROOTS_FRESH = 0,
   KB_MGMT_ROOTS_RECOVERED = 1,
   KB_MGMT_ROOTS_FINAL = 2,
   KB_MGMT_ROOTS_RETRY = 3,
   KB_MGMT_ROOTS_SEALED = 4,
   KB_MGMT_ROOTS_CONFLICT = 5,
   KB_MGMT_ROOTS_INTEGRITY = 6,
} kb_mgmt_roots_result_t;

typedef struct
{
   int64_t version;
   uint8_t wrapped_dek[VAULT_WRAPPED_DEK_LEN];
   uint8_t nonce[VAULT_GCM_NONCE_LEN];
   uint8_t ciphertext[KB_MGMT_ROOT_SECRET_MAX];
   size_t ciphertext_len;
   uint8_t tag[VAULT_GCM_TAG_LEN];
} kb_mgmt_root_envelope_t;

typedef struct
{
   kb_mgmt_root_kind_t kind;
   kb_mgmt_root_phase_t phase;
   char custody_key_id[KB_MGMT_ROOT_CUSTODY_ID_MAX + 1];
   char bootstrap_id[KB_MGMT_ROOT_BOOTSTRAP_LEN + 1];
   char wire_id[KB_MGMT_TOKEN_KID_MAX + 1];
   uint8_t public_key[KB_MGMT_TOKEN_MODULUS_LEN];
   size_t public_key_len;
   uint8_t public_digest[32];
   uint8_t jwk_digest[32]; /* zero for the manifest root */
   uint64_t seal_epoch;
   uint8_t hwm1_attestation[KB_MGMT_ROOT_ATTEST_MAX];
   size_t hwm1_attestation_len;
   uint8_t hwm2_attestation[KB_MGMT_ROOT_ATTEST_MAX];
   size_t hwm2_attestation_len;
   kb_mgmt_root_envelope_t v1;
   kb_mgmt_root_envelope_t v2;
   uint8_t v1_digest[32];
   uint8_t v2_digest[32];
} kb_mgmt_root_record_t;

typedef struct
{
   int bound;
   char custody_key_id[KB_MGMT_ROOT_CUSTODY_ID_MAX + 1];
   char helper[129];
   char verifier_domain[129];
   uint8_t identity_digest[32];
   uint8_t hwm1_attestation[KB_MGMT_ROOT_ATTEST_MAX];
   size_t hwm1_attestation_len;
} kb_mgmt_publication_root_t;

typedef struct
{
   kb_mgmt_root_db_result_t (*inspect_root)(void *, kb_mgmt_root_kind_t, const char *,
                                            kb_mgmt_root_record_t *);
   kb_mgmt_root_db_result_t (*stage_root)(void *, const kb_mgmt_root_record_t *);
   kb_mgmt_root_db_result_t (*record_cas)(void *, const kb_mgmt_root_record_t *, const uint8_t *,
                                          size_t);
   kb_mgmt_root_db_result_t (*finalize_root)(void *, const kb_mgmt_root_record_t *);
   kb_mgmt_root_db_result_t (*inspect_publication)(void *, kb_mgmt_publication_root_t *);
   kb_mgmt_root_db_result_t (*bind_publication)(void *, const kb_mgmt_publication_root_t *);
   void *ctx;
} kb_mgmt_roots_db_t;

typedef struct
{
   const char *token_custody_key_id;
   const char *manifest_custody_key_id;
   const char *publication_custody_key_id;
   const char *publication_helper;
   const char *publication_verifier_domain;
   uint8_t publication_identity_digest[32];
} kb_mgmt_roots_config_t;

/* All encoders emit one exact compact JSON representation and clear output on error. */
int kb_mgmt_manifest_wire_id(const uint8_t public_key[32], char *out, size_t cap);
int kb_mgmt_public_bundle(const uint8_t *token_modulus, size_t token_modulus_len,
                          const uint8_t manifest_public[32],
                          const uint8_t publication_identity_digest[32], char *out, size_t cap,
                          size_t *out_len);
int kb_mgmt_public_bundle_validate(const char *bundle, size_t bundle_len);

int kb_mgmt_root_bootstrap_id(kb_mgmt_root_kind_t kind, const char *custody_key_id,
                              char out[KB_MGMT_ROOT_BOOTSTRAP_LEN + 1]);
int kb_mgmt_root_aad(kb_mgmt_root_kind_t kind, int64_t version, uint8_t *out, size_t cap,
                     size_t *out_len);
int kb_mgmt_root_envelope_digest(kb_mgmt_root_kind_t kind, const kb_mgmt_root_envelope_t *envelope,
                                 uint8_t out[32]);

/* Offline owner-only core. Only FRESH emits a bundle. Export is mutation-free. */
kb_mgmt_roots_result_t kb_mgmt_token_roots_provision(const kb_mgmt_roots_config_t *,
                                                     const kb_mgmt_roots_db_t *, char *bundle,
                                                     size_t bundle_cap, size_t *bundle_len);
kb_mgmt_roots_result_t kb_mgmt_token_roots_export(const kb_mgmt_roots_config_t *,
                                                  const kb_mgmt_roots_db_t *, char *bundle,
                                                  size_t bundle_cap, size_t *bundle_len);

#endif
