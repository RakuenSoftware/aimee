/* Pure, domain-separated P5-B2c custody transcript builders. KB-internal. */
#ifndef AIMEE_KB_MANAGEMENT_CERT_BINDING_H
#define AIMEE_KB_MANAGEMENT_CERT_BINDING_H

#include "kb_management_cert_lifecycle.h"

/* Transcript APIs require cap <= this bound and clear that entire buffer on
 * ordinary validation/encoding failure. Aliased input/output is rejected
 * without modifying either object. */
#define KB_MANAGEMENT_CERT_TRANSCRIPT_MAX 4096U

typedef struct
{
   char installation_id[33];
   char lineage_id[33];
   char operation_id[65];
   char authority_id[33];
   char storage_id[33];
   int64_t generation;
   kb_workload_provider_kind_t provider_kind;
   char workload_issuer[sizeof(((kb_workload_identity_t *)0)->issuer)];
   char workload_subject[sizeof(((kb_workload_identity_t *)0)->subject)];
   uint8_t binding_digest[32];
   uint8_t proof_anchor[32];
   uint8_t custody_anchor[32];
   uint8_t csr_digest[32];
   uint8_t csr_spki_digest[32];
   uint8_t nonce[32];
} kb_management_cert_intent_binding_t;

typedef struct
{
   kb_management_cert_intent_binding_t intent;
   char ca_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   uint8_t ca_fingerprint[32];
   char leaf_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char leaf_serial_norm[DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t leaf_fingerprint[32];
   uint8_t leaf_spki_digest[32];
   int64_t not_before_epoch;
   int64_t not_after_epoch;
   uint8_t public_bundle_digest[32];
} kb_management_cert_candidate_binding_t;

int kb_management_cert_attest_transcript(const char installation_id[33],
                                         kb_workload_provider_kind_t, uint8_t *, size_t, size_t *);
int kb_management_cert_intent_transcript(const kb_management_cert_intent_binding_t *, uint8_t *,
                                         size_t, size_t *);
int kb_management_cert_candidate_transcript(const kb_management_cert_candidate_binding_t *,
                                            uint8_t *, size_t, size_t *);
int kb_management_cert_attest_binding(const char installation_id[33], kb_workload_provider_kind_t,
                                      uint8_t[32]);
int kb_management_cert_intent_binding(const kb_management_cert_intent_binding_t *, uint8_t[32]);
int kb_management_cert_candidate_binding(const kb_management_cert_candidate_binding_t *,
                                         uint8_t[32]);

#endif
