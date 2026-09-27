#ifndef AIMEE_KB_STORE_MANAGEMENT_JWKS_PUBLICATION_H
#define AIMEE_KB_STORE_MANAGEMENT_JWKS_PUBLICATION_H

#include "kb_mgmt_jwks_publication.h"

#include <stddef.h>
#include <stdint.h>

typedef struct
{
   uint64_t seal_epoch;
   kb_mgmt_root_envelope_t envelope;
   uint8_t hwm_attestation[KB_MGMT_ROOT_ATTEST_MAX];
   size_t hwm_attestation_len;
   int newly_admitted;
} kb_store_management_jwks_admission_t;

typedef struct
{
   void *connection;
   int barrier_lock_held;
   int publication_lock_held;
   int provider_binding_set;
   int snapshot_valid;
   char provider_helper[129];
   char provider_verifier_domain[129];
   uint8_t provider_identity_digest[32];
   kb_mgmt_jwks_roots_t roots;
} kb_store_management_jwks_publication_ctx_t;

int kb_store_management_jwks_publication_open(kb_store_management_jwks_publication_ctx_t *,
                                              const char *conninfo, char *errbuf, size_t errlen);
void kb_store_management_jwks_publication_close(kb_store_management_jwks_publication_ctx_t *);
int kb_store_management_jwks_publication_set_provider_binding(
    kb_store_management_jwks_publication_ctx_t *, const char *helper, const char *verifier_domain,
    const uint8_t identity_digest[32]);
int kb_store_management_jwks_publication_bind(kb_store_management_jwks_publication_ctx_t *,
                                              kb_mgmt_jwks_callbacks_t *);

/* Commit the immutable manifest-key-use admission before returning the exact
 * admitted v2 envelope. Exact replay returns the same envelope with
 * newly_admitted=0. */
kb_mgmt_jwks_db_result_t kb_store_management_jwks_manifest_key_admit(
    kb_store_management_jwks_publication_ctx_t *, const char *use_id, uint64_t generation,
    const char *candidate_id, const kb_mgmt_root_record_t *manifest,
    const uint8_t payload_digest[32], kb_store_management_jwks_admission_t *);

#endif
