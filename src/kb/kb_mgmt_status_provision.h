#ifndef AIMEE_KB_MGMT_STATUS_PROVISION_H
#define AIMEE_KB_MGMT_STATUS_PROVISION_H

#include "vault_crypto.h"

#include <stddef.h>
#include <stdint.h>

#define KB_MGMT_STATUS_PROVISION_CUSTODY_ID_MAX   600
#define KB_MGMT_STATUS_PROVISION_WIRE_ID_MAX      64
#define KB_MGMT_STATUS_PROVISION_PUBLIC_LEN       32
#define KB_MGMT_STATUS_PROVISION_DIGEST_LEN       32
#define KB_MGMT_STATUS_PROVISION_ATTEST_MAX       512
#define KB_MGMT_STATUS_PROVISION_BOOTSTRAP_ID_LEN 64

typedef enum
{
   KB_MGMT_STATUS_PROVISION_EMPTY = 0,
   KB_MGMT_STATUS_PROVISION_STAGED = 1,
   KB_MGMT_STATUS_PROVISION_ENABLED = 2,
} kb_mgmt_status_provision_phase_t;

typedef enum
{
   KB_MGMT_STATUS_PROVISION_DB_OK = 0,
   KB_MGMT_STATUS_PROVISION_DB_RETRY = 1,
   KB_MGMT_STATUS_PROVISION_DB_SEALED = 2,
   KB_MGMT_STATUS_PROVISION_DB_CONFLICT = 3,
   KB_MGMT_STATUS_PROVISION_DB_INTEGRITY = 4,
} kb_mgmt_status_provision_db_result_t;

typedef enum
{
   /* A new key was finalized. Only this result carries publishable output. */
   KB_MGMT_STATUS_PROVISION_FRESH = 0,
   /* An exact staged operation was recovered and finalized; output is suppressed. */
   KB_MGMT_STATUS_PROVISION_RECOVERED = 1,
   /* A key is already enabled. Provisioning is overwrite-refusing. */
   KB_MGMT_STATUS_PROVISION_CONFLICT = 2,
   /* A transient database, custody, HWM, memory-protection, or seal failure. */
   KB_MGMT_STATUS_PROVISION_RETRY = 3,
   /* The durable primary is sealed. */
   KB_MGMT_STATUS_PROVISION_SEALED = 4,
   /* Persisted state, envelope, binding, or anchor state is impossible/mismatched. */
   KB_MGMT_STATUS_PROVISION_INTEGRITY = 5,
} kb_mgmt_status_provision_result_t;

typedef struct
{
   int64_t version;
   uint8_t wrapped_dek[VAULT_WRAPPED_DEK_LEN];
   uint8_t nonce[VAULT_GCM_NONCE_LEN];
   uint8_t ciphertext[KB_MGMT_STATUS_PROVISION_PUBLIC_LEN];
   size_t ciphertext_len;
   uint8_t tag[VAULT_GCM_TAG_LEN];
} kb_mgmt_status_provision_envelope_t;

typedef struct
{
   kb_mgmt_status_provision_phase_t phase;
   char bootstrap_id[KB_MGMT_STATUS_PROVISION_BOOTSTRAP_ID_LEN + 1];
   char custody_key_id[KB_MGMT_STATUS_PROVISION_CUSTODY_ID_MAX + 1];
   char wire_key_id[KB_MGMT_STATUS_PROVISION_WIRE_ID_MAX + 1];
   uint8_t public_key[KB_MGMT_STATUS_PROVISION_PUBLIC_LEN];
   uint8_t public_key_digest[KB_MGMT_STATUS_PROVISION_DIGEST_LEN];
   uint64_t seal_epoch;
   uint8_t hwm1_attestation[KB_MGMT_STATUS_PROVISION_ATTEST_MAX];
   size_t hwm1_attestation_len;
   uint8_t hwm2_attestation[KB_MGMT_STATUS_PROVISION_ATTEST_MAX];
   size_t hwm2_attestation_len;
   kb_mgmt_status_provision_envelope_t v1;
   kb_mgmt_status_provision_envelope_t v2;
   uint8_t v1_digest[KB_MGMT_STATUS_PROVISION_DIGEST_LEN];
   uint8_t v2_digest[KB_MGMT_STATUS_PROVISION_DIGEST_LEN];
} kb_mgmt_status_provision_record_t;

typedef struct
{
   kb_mgmt_status_provision_db_result_t (*inspect)(void *ctx, const char *custody_key_id,
                                                   kb_mgmt_status_provision_record_t *record);
   kb_mgmt_status_provision_db_result_t (*stage)(void *ctx,
                                                 const kb_mgmt_status_provision_record_t *record);
   kb_mgmt_status_provision_db_result_t (*prepare_activation)(
       void *ctx, const kb_mgmt_status_provision_record_t *record);
   kb_mgmt_status_provision_db_result_t (*finalize)(void *ctx,
                                                    const kb_mgmt_status_provision_record_t *record,
                                                    const uint8_t *hwm_attestation,
                                                    size_t hwm_attestation_len);
   void *ctx;
} kb_mgmt_status_provision_db_t;

typedef struct
{
   char custody_key_id[KB_MGMT_STATUS_PROVISION_CUSTODY_ID_MAX + 1];
   char wire_key_id[KB_MGMT_STATUS_PROVISION_WIRE_ID_MAX + 1];
   uint8_t public_key[KB_MGMT_STATUS_PROVISION_PUBLIC_LEN];
} kb_mgmt_status_provision_output_t;

/* Owner-only orchestration core. The caller binds KMS custody before entry.
 * Only FRESH populates output; every other result leaves it all-zero. */
kb_mgmt_status_provision_result_t
kb_mgmt_status_provision(const char *custody_key_id, const kb_mgmt_status_provision_db_t *database,
                         kb_mgmt_status_provision_output_t *output);

/* Pure helpers used by the fixed PostgreSQL adapter and focused tests. */
int kb_mgmt_status_provision_wire_id(const uint8_t public_key[KB_MGMT_STATUS_PROVISION_PUBLIC_LEN],
                                     char out[KB_MGMT_STATUS_PROVISION_WIRE_ID_MAX + 1]);
int kb_mgmt_status_provision_bootstrap_id(const char *custody_key_id,
                                          char out[KB_MGMT_STATUS_PROVISION_BOOTSTRAP_ID_LEN + 1]);
int kb_mgmt_status_provision_envelope_digest(const kb_mgmt_status_provision_envelope_t *envelope,
                                             uint8_t out[KB_MGMT_STATUS_PROVISION_DIGEST_LEN]);

#endif
