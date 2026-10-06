#ifndef AIMEE_KB_STORE_MANAGEMENT_JWKS_RUNTIME_H
#define AIMEE_KB_STORE_MANAGEMENT_JWKS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define KB_STORE_MANAGEMENT_JWKS_ENVELOPE_MAX 3072

typedef enum
{
   KB_STORE_MANAGEMENT_JWKS_RUNTIME_OK = 0,
   KB_STORE_MANAGEMENT_JWKS_RUNTIME_DENIED = 1,
   KB_STORE_MANAGEMENT_JWKS_RUNTIME_UNAVAILABLE = 2,
   KB_STORE_MANAGEMENT_JWKS_RUNTIME_INTEGRITY = 3,
} kb_store_management_jwks_runtime_result_t;

typedef struct
{
   int64_t generation;
   int64_t valid_from;
   int64_t valid_until;
   char candidate_id[65];
   char envelope[KB_STORE_MANAGEMENT_JWKS_ENVELOPE_MAX];
   size_t envelope_len;
   unsigned char envelope_sha256[32];
   unsigned char manifest_sha256[32];
   unsigned char jwks_sha256[32];
   unsigned char payload_sha256[32];
   unsigned char hwm2_attestation_digest[32];
} kb_store_management_jwks_runtime_record_t;

kb_store_management_jwks_runtime_result_t
kb_store_management_jwks_runtime_fetch(const char *issuer, const char *serial_norm,
                                       const char *fingerprint,
                                       kb_store_management_jwks_runtime_record_t *out);

#endif
