#ifndef AIMEE_KB_MANAGEMENT_CERT_CRYPTO_H
#define AIMEE_KB_MANAGEMENT_CERT_CRYPTO_H

#include "kb_management_cert_lifecycle.h"

typedef struct
{
   uint8_t key_der[4096];
   size_t key_der_len;
   uint8_t csr_der[4096];
   size_t csr_der_len;
   char csr_pem[4096];
   size_t csr_pem_len;
   uint8_t csr_digest[32];
   uint8_t csr_spki_digest[32];
} kb_management_cert_key_material_t;

typedef struct
{
   uint8_t leaf_der[4096];
   size_t leaf_der_len;
   uint8_t ca_der[4096];
   size_t ca_der_len;
   char ca_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   uint8_t ca_fingerprint[32];
   char leaf_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char leaf_serial_norm[DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t leaf_fingerprint[32];
   uint8_t leaf_spki_digest[32];
   int64_t not_before_epoch;
   int64_t not_after_epoch;
} kb_management_cert_verified_t;

int kb_management_cert_key_generate(kb_management_cert_key_material_t *);
int kb_management_cert_key_intent_verify(const uint8_t *, size_t, const uint8_t *, size_t,
                                         kb_management_cert_key_material_t *);
int kb_management_cert_leaf_verify(const kb_management_cert_key_material_t *, const char *,
                                   const char *, kb_management_cert_verified_t *);
int kb_management_cert_bundle_to_pem(const uint8_t *, size_t, const uint8_t *, size_t,
                                     const uint8_t *, size_t, kb_management_cert_bundle_t *);
int kb_management_cert_sha256(const void *, size_t, uint8_t[32]);
void kb_management_cert_key_material_clear(kb_management_cert_key_material_t *);

#endif
