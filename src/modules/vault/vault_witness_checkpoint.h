#ifndef AIMEE_VAULT_WITNESS_CHECKPOINT_H
#define AIMEE_VAULT_WITNESS_CHECKPOINT_H

#include <stddef.h>
#include <stdint.h>

/* P7-witness-e1: signed checkpoint format, verification, and continuity verdict.
 *
 * A checkpoint commits to every shard head at an instant via the SMT root
 * (vault_witness_merkle). It is signed under a vault-held key and verified offline
 * by a consumer holding only the emitted bytes plus an out-of-band trust anchor —
 * never a key fetched from the emitting host, which an attacker also controls.
 *
 * Verification is a pure function over bytes plus the anchor set: it never
 * consults a database, so a compromised database cannot influence the verdict.
 * Continuity (does this checkpoint link to the one before it) is a SEPARATE
 * verdict from validity, because emission gaps are normal, not tampering.
 *
 * E1 defines the format and the verify/continuity entry points; E2 owns the
 * cadence, the fenced signer, and persist-before-emit. This module is pure and
 * production-uninvoked in E1.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define VAULT_WITNESS_CHECKPOINT_LABEL "aimee-vault-witness-checkpoint-v1"
#define VAULT_WITNESS_SIGNER_KEY_ID_LEN 16
#define VAULT_WITNESS_ED25519_SIG_LEN 64
#define VAULT_WITNESS_ED25519_PUB_LEN 32
#define VAULT_WITNESS_ED25519_PRIV_LEN 32
#define VAULT_WITNESS_CP_CREATED_AT_MAX 40

/* Signature algorithm identifiers. Only Ed25519 is defined in E1. */
typedef enum
{
   VAULT_WITNESS_SIG_ED25519 = 1
} vault_witness_sig_alg_t;

typedef struct vault_witness_checkpoint
{
   uint16_t version;
   uint64_t seq;
   uint8_t root[32];
   int has_predecessor;               /* 0 for the first checkpoint */
   uint8_t predecessor_digest[32];    /* all-zero when has_predecessor == 0 */
   uint64_t shard_count;
   uint8_t leaf_snapshot_digest[32];  /* commitment to the emitted leaf snapshot */
   uint8_t signer_key_id[VAULT_WITNESS_SIGNER_KEY_ID_LEN];
   uint16_t sig_alg;                  /* vault_witness_sig_alg_t */
   uint16_t sig_version;
   char created_at[VAULT_WITNESS_CP_CREATED_AT_MAX + 1];
   uint8_t signature[VAULT_WITNESS_ED25519_SIG_LEN];
} vault_witness_checkpoint_t;

/* One trusted verification key. `revoked` != 0 means a checkpoint signed by this
 * key is rejected even when the signature is mathematically valid — a key
 * compromised before rotation must not keep validating forged checkpoints. */
typedef struct vault_witness_anchor
{
   uint8_t key_id[VAULT_WITNESS_SIGNER_KEY_ID_LEN];
   uint8_t ed25519_pub[VAULT_WITNESS_ED25519_PUB_LEN];
   int revoked;
} vault_witness_anchor_t;

typedef enum
{
   VAULT_WITNESS_CP_OK = 0,       /* signature valid, key known and not revoked */
   VAULT_WITNESS_CP_MALFORMED,    /* structurally invalid checkpoint */
   VAULT_WITNESS_CP_UNKNOWN_KEY,  /* signer_key_id not in the anchor set */
   VAULT_WITNESS_CP_REVOKED_KEY,  /* signer_key_id present but revoked */
   VAULT_WITNESS_CP_BAD_SIG       /* key known, signature does not verify */
} vault_witness_cp_verdict_t;

typedef enum
{
   VAULT_WITNESS_CONTINUITY_OK = 0,   /* predecessor digest matches the expected one */
   VAULT_WITNESS_CONTINUITY_UNPROVEN, /* no expected predecessor supplied (a gap) */
   VAULT_WITNESS_CONTINUITY_BROKEN    /* expected predecessor supplied and mismatched */
} vault_witness_continuity_t;

/* Canonical signable body (everything but the signature), domain-separated and
 * length-prefixed. Signing and verification both operate on these exact bytes.
 * Returns 0 on success, -1 on invalid checkpoint or small buffer. */
int vault_witness_checkpoint_signable(const vault_witness_checkpoint_t *cp, uint8_t *out,
                                      size_t cap, size_t *out_len);

/* SHA-256 of the signable body — the checkpoint's stable identity digest, used as
 * a predecessor reference by later checkpoints. Returns 0/-1. */
int vault_witness_checkpoint_digest(const vault_witness_checkpoint_t *cp, uint8_t digest[32]);

/* Largest a full encoded checkpoint can be (signable body + 64-byte signature). */
#define VAULT_WITNESS_CHECKPOINT_WIRE_MAX 640

/* Full wire form for emission and offline consumption: the canonical signable body
 * followed by the 64-byte signature. Deterministic. Returns 0 on success and sets
 * *out_len, -1 on invalid checkpoint or small buffer. */
int vault_witness_checkpoint_encode(const vault_witness_checkpoint_t *cp, uint8_t *out, size_t cap,
                                    size_t *out_len);

/* Decode and validate a full wire checkpoint into `cp`. Rejects a bad label,
 * wrong version, unknown sig_alg, declared lengths that do not fit, a set
 * has_predecessor with a zero digest mismatch, or a missing signature. The decoded
 * checkpoint can be re-verified with vault_witness_checkpoint_verify. Returns 0/-1;
 * `cp` is cleansed on failure. */
int vault_witness_checkpoint_decode(const uint8_t *wire, size_t wire_len,
                                    vault_witness_checkpoint_t *cp);

/* Sign the checkpoint's signable body with a raw Ed25519 private key, filling
 * cp->signature and setting sig_alg = Ed25519. A helper for tests and E2's signer;
 * not a production caller in E1. Returns 0/-1. */
int vault_witness_checkpoint_sign_ed25519(vault_witness_checkpoint_t *cp,
                                          const uint8_t priv[VAULT_WITNESS_ED25519_PRIV_LEN]);

/* Verify a checkpoint against the anchor set: resolve signer_key_id, reject if
 * unknown or revoked, then check the Ed25519 signature over the signable body. */
vault_witness_cp_verdict_t vault_witness_checkpoint_verify(const vault_witness_checkpoint_t *cp,
                                                           const vault_witness_anchor_t *anchors,
                                                           size_t n_anchors);

/* Continuity verdict, independent of validity. `expected_predecessor` may be NULL
 * (the consumer holds a gapped set): then the verdict is UNPROVEN — a work item,
 * never a clean pass. If supplied, OK iff it matches cp->predecessor_digest. */
vault_witness_continuity_t
vault_witness_checkpoint_continuity(const vault_witness_checkpoint_t *cp,
                                    const uint8_t *expected_predecessor);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_VAULT_WITNESS_CHECKPOINT_H */
