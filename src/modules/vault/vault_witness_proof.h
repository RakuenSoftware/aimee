#ifndef AIMEE_VAULT_WITNESS_PROOF_H
#define AIMEE_VAULT_WITNESS_PROOF_H

#include <stddef.h>
#include <stdint.h>

#include "vault_witness_merkle.h"
#include "vault_witness_record.h"

/* P7-witness-e2: emitted inclusion proof. Carries the shard IDENTITY (so the
 * verifier recomputes the SMT key and leaf itself, never trusting a caller-supplied
 * one), the checkpoint sequence whose signed root it proves against, and the
 * depth-64 sibling path. Emitted alongside checkpoints on the log path so a
 * consumer holding a signed checkpoint can confirm a specific shard head was
 * committed by it.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define VAULT_WITNESS_PROOF_WIRE_MAX \
   (32 + 2 + VAULT_WITNESS_TENANT_MAX + 2 + VAULT_WITNESS_PROVIDER_MAX + 8 + 32 + \
    VAULT_WITNESS_SMT_DEPTH * 32)

typedef struct
{
   uint64_t checkpoint_seq;
   char tenant[VAULT_WITNESS_TENANT_MAX + 1];
   char provider[VAULT_WITNESS_PROVIDER_MAX + 1];
   uint64_t sequence;
   uint8_t head_hash[32];
   uint8_t path[VAULT_WITNESS_SMT_DEPTH][32];
} vault_witness_proof_t;

/* Deterministic wire form. Returns 0 and sets *out_len, -1 on invalid input or
 * small buffer. */
int vault_witness_proof_encode(const vault_witness_proof_t *p, uint8_t *out, size_t cap,
                               size_t *out_len);

/* Decode + validate. Rejects bad magic/version, a shard key that is empty or
 * over-long, and any declared length that does not fit. Returns 0/-1; `p` is
 * cleansed on failure. */
int vault_witness_proof_decode(const uint8_t *wire, size_t wire_len, vault_witness_proof_t *p);

/* Verify the proof authenticates its shard head under `root` (the root of the
 * checkpoint identified by checkpoint_seq — the caller supplies the matching
 * checkpoint's root). Recomputes key and leaf from the identity. Returns 1 if
 * included, 0 otherwise. */
int vault_witness_proof_verify(const vault_witness_proof_t *p, const uint8_t root[32]);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_VAULT_WITNESS_PROOF_H */
