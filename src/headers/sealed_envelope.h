/* Shared 4 KiB vault/root AES-GCM envelope storage. This is an in-memory
 * shape, not a wire representation; callers retain purpose-specific digests. */
#ifndef AIMEE_SEALED_ENVELOPE_H
#define AIMEE_SEALED_ENVELOPE_H 1
#include <stddef.h>
#include <stdint.h>
#define AIMEE_SEALED_ENVELOPE_CIPHERTEXT_MAX 4096
typedef struct
{
   int64_t version;
   uint8_t wrapped_dek[40];
   uint8_t nonce[12];
   uint8_t ciphertext[AIMEE_SEALED_ENVELOPE_CIPHERTEXT_MAX];
   size_t ciphertext_len;
   uint8_t tag[16];
} aimee_sealed_envelope_t;
#endif
