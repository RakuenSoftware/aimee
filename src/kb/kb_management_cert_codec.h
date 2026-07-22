/* Strict P5-B2c binary formats. This header is KB-internal. */
#ifndef AIMEE_KB_MANAGEMENT_CERT_CODEC_H
#define AIMEE_KB_MANAGEMENT_CERT_CODEC_H

#include "kb_management_cert_lifecycle.h"

#define KB_MANAGEMENT_CERT_CANDIDATE_MAX (2048U + KB_WORKLOAD_WRAP_CAP)

typedef struct
{
   const uint8_t *key_der;
   size_t key_der_len;
   const uint8_t *csr_der;
   size_t csr_der_len;
} kb_management_cert_key_intent_view_t;

typedef struct
{
   const uint8_t *key_der;
   size_t key_der_len;
   const uint8_t *leaf_der;
   size_t leaf_der_len;
   const uint8_t *ca_der;
   size_t ca_der_len;
} kb_management_cert_bundle_view_t;

typedef struct
{
   char installation_id[33];
   char lineage_id[33];
   char operation_id[65];
   char authority_id[33];
   char storage_id[33];
   int64_t generation;
   kb_workload_provider_kind_t provider_kind;
   uint8_t nonce[32];
   uint8_t binding_digest[32];
   uint8_t csr_digest[32];
   uint8_t csr_spki_digest[32];
   uint8_t custody_binding_digest[32];
   const uint8_t *ciphertext;
   size_t ciphertext_len;
} kb_management_cert_intent_view_t;

typedef struct
{
   char installation_id[33];
   char lineage_id[33];
   char operation_id[65];
   char authority_id[33];
   char storage_id[33];
   int64_t generation;
   kb_workload_provider_kind_t provider_kind;
   uint8_t nonce[32];
   uint8_t binding_digest[32];
   uint8_t csr_digest[32];
   uint8_t csr_spki_digest[32];
   uint8_t public_bundle_digest[32];
   uint8_t custody_binding_digest[32];
   /* issuer remains the leaf issuer for source compatibility. */
   char issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char ca_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   uint8_t ca_fingerprint[32];
   char serial_norm[DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t fingerprint[32];
   uint8_t spki_digest[32];
   int64_t not_before_epoch;
   int64_t not_after_epoch;
   const uint8_t *ciphertext;
   size_t ciphertext_len;
} kb_management_cert_candidate_view_t;

typedef enum
{
   KB_MANAGEMENT_CERT_ISSUE_INITIAL = 1,
   KB_MANAGEMENT_CERT_ISSUE_RENEWAL = 2
} kb_management_cert_issue_kind_t;

typedef struct
{
   char installation_id[33];
   char lineage_id[33];
   char operation_id[65];
   char authority_id[33];
   int64_t generation;
   kb_management_cert_issue_kind_t issue_kind;
   uint8_t binding_digest[32];
   uint8_t intent_record_digest[32];
} kb_management_cert_pending_manifest_t;

typedef struct
{
   char operation_id[65];
   int64_t generation;
   uint8_t public_bundle_digest[32];
} kb_management_cert_manifest_t;

int kb_management_cert_key_intent_encode(const void *, size_t, const void *, size_t, uint8_t *,
                                         size_t, size_t *);
int kb_management_cert_key_intent_decode(const void *, size_t,
                                         kb_management_cert_key_intent_view_t *);
int kb_management_cert_bundle_encode(const void *, size_t, const void *, size_t, const void *,
                                     size_t, uint8_t *, size_t, size_t *);
int kb_management_cert_bundle_decode(const void *, size_t, kb_management_cert_bundle_view_t *);
int kb_management_cert_intent_encode(const kb_management_cert_intent_view_t *, uint8_t *, size_t,
                                     size_t *);
int kb_management_cert_intent_decode(const void *, size_t, kb_management_cert_intent_view_t *);
int kb_management_cert_candidate_encode(const kb_management_cert_candidate_view_t *, uint8_t *,
                                        size_t, size_t *);
int kb_management_cert_candidate_decode(const void *, size_t,
                                        kb_management_cert_candidate_view_t *);
int kb_management_cert_manifest_encode(const kb_management_cert_manifest_t *, uint8_t *, size_t,
                                       size_t *);
int kb_management_cert_manifest_decode(const void *, size_t, kb_management_cert_manifest_t *);
int kb_management_cert_pending_encode(const kb_management_cert_pending_manifest_t *, uint8_t *,
                                      size_t, size_t *);
int kb_management_cert_pending_decode(const void *, size_t,
                                      kb_management_cert_pending_manifest_t *);

#endif
