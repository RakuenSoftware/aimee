#ifndef AIMEE_VAULT_RESEAL_RECEIPT_H
#define AIMEE_VAULT_RESEAL_RECEIPT_H

#include <stddef.h>
#include <stdint.h>

#define VAULT_RESEAL_RECEIPT_V1_LEN    208
#define VAULT_RESEAL_OPERATION_ID_LEN  16
#define VAULT_RESEAL_OPERATION_HEX_LEN 32

typedef struct vault_tpm2_reseal_receipt
{
   uint8_t operation_id[VAULT_RESEAL_OPERATION_ID_LEN];
   uint64_t old_generation;
   uint64_t new_generation;
   uint8_t predecessor_digest[32];
   uint8_t capsule_digest[32];
   uint8_t future_digest[32];
   uint8_t new_kek_digest[32];
   uint8_t manifest_digest[32];
} vault_tpm2_reseal_receipt_t;

/* Canonical, compiler-layout-independent persistence format for a TPM2 reseal
 * receipt. These helpers are available in the default non-TPM build. */
int vault_reseal_receipt_encode(const vault_tpm2_reseal_receipt_t *receipt,
                                uint8_t out[VAULT_RESEAL_RECEIPT_V1_LEN]);
int vault_reseal_receipt_decode(const uint8_t *wire, size_t wire_len,
                                vault_tpm2_reseal_receipt_t *receipt);
int vault_reseal_receipt_digest(const uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN],
                                uint8_t digest[32]);
int vault_reseal_receipt_equal(const vault_tpm2_reseal_receipt_t *a,
                               const vault_tpm2_reseal_receipt_t *b);

/* The SQL operation ID is exactly 32 lowercase hex characters plus NUL. */
int vault_reseal_operation_id_to_hex(const uint8_t operation_id[VAULT_RESEAL_OPERATION_ID_LEN],
                                     char out[VAULT_RESEAL_OPERATION_HEX_LEN + 1]);
int vault_reseal_operation_id_from_hex(const char *hex,
                                       uint8_t operation_id[VAULT_RESEAL_OPERATION_ID_LEN]);

#endif
