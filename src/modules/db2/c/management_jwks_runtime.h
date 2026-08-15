#ifndef AIMEE_DB2_MANAGEMENT_JWKS_RUNTIME_H
#define AIMEE_DB2_MANAGEMENT_JWKS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define DB2_MANAGEMENT_JWKS_ENVELOPE_MAX 3072

typedef enum
{
   DB2_MANAGEMENT_JWKS_RUNTIME_OK = 0,
   DB2_MANAGEMENT_JWKS_RUNTIME_DENIED = 1,
   DB2_MANAGEMENT_JWKS_RUNTIME_UNAVAILABLE = 2,
   DB2_MANAGEMENT_JWKS_RUNTIME_INTEGRITY = 3,
} db2_management_jwks_runtime_result_t;

typedef struct
{
   int64_t generation;
   int64_t valid_from;
   int64_t valid_until;
   char candidate_id[65];
   char envelope[DB2_MANAGEMENT_JWKS_ENVELOPE_MAX];
   size_t envelope_len;
   unsigned char envelope_sha256[32];
   unsigned char manifest_sha256[32];
   unsigned char jwks_sha256[32];
   unsigned char payload_sha256[32];
   unsigned char hwm2_attestation_digest[32];
} db2_management_jwks_runtime_record_t;

db2_management_jwks_runtime_result_t
db2_management_jwks_runtime_fetch(const char *issuer, const char *serial_norm,
                                  const char *fingerprint,
                                  db2_management_jwks_runtime_record_t *out);

#endif
