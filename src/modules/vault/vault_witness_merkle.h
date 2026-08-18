#ifndef AIMEE_VAULT_WITNESS_MERKLE_H
#define AIMEE_VAULT_WITNESS_MERKLE_H

#include <stddef.h>
#include <stdint.h>

/* P7-witness-e1: sparse Merkle tree over shard heads for the signed checkpoint.
 *
 * The commitment is an SMT of fixed depth 64, keyed by the leading 8 bytes of
 * SHA-256 over the packed shard key (see vault_witness_shard_key_hash). Fixed
 * topology means advancing one shard touches one root-to-leaf path and proofs are
 * stable; a shallower key would let two shards share a leaf, which would let one
 * tenant's head stand in for another's. Empty subtrees have precomputed constant
 * hashes per level and are never materialized, so a root over N occupied leaves
 * costs O(N * depth), not O(2^64).
 *
 * Pure and production-uninvoked in E1: no database, no I/O, no scheduler.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define VAULT_WITNESS_SMT_DEPTH 64

/* Domain-separation labels. Any change is a breaking format change. */
#define VAULT_WITNESS_SMT_LEAF_LABEL "aimee-vault-witness-smt-leaf-v1"
#define VAULT_WITNESS_SMT_NODE_LABEL "aimee-vault-witness-smt-node-v1"
#define VAULT_WITNESS_SMT_EMPTY_LABEL "aimee-vault-witness-smt-empty-v1"

/* One occupied leaf: the 8-byte shard-key hash (position) and the 32-byte leaf
 * commitment (see vault_witness_leaf_hash). */
typedef struct vault_witness_leaf
{
   uint8_t key[8];
   uint8_t hash[32];
} vault_witness_leaf_t;

/* Commit one shard's head to a leaf hash:
 *   SHA-256( LEAF_LABEL || pack_text(tenant) || pack_text(provider)
 *            || u64_be(sequence) || head_hash[32] )
 * The leaf binds the full shard identity, so a leaf lifted to another key-hash
 * position fails to reproduce the root it was generated against. Returns 0 on
 * success, -1 on invalid input. */
int vault_witness_leaf_hash(const char *tenant, const char *provider, uint64_t sequence,
                            const uint8_t head_hash[32], uint8_t out[32]);

/* Compute the SMT root over `n` leaves. Leaves MUST be sorted strictly ascending
 * by `key` (memcmp order); a duplicate key is a collision and is rejected (two
 * shards would share a leaf). n==0 yields the all-empty root. Returns 0 on
 * success, -1 on unsorted input, a duplicate key, or n over the ceiling. */
int vault_witness_merkle_root(const vault_witness_leaf_t *leaves, size_t n, uint8_t root[32]);

/* Generate the depth-64 inclusion proof for leaves[index]. `proof` is filled so
 * that proof[b] is the sibling subtree root at the node whose discriminating bit
 * index is b (b==0 is the root's MSB split, b==63 the deepest). Same sort
 * precondition as _root. Returns 0 on success, -1 on invalid input. */
int vault_witness_merkle_proof(const vault_witness_leaf_t *leaves, size_t n, size_t index,
                               uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32]);

/* Verify that (key, leaf_hash) is included under `root` via `proof`. Bit b of the
 * key (b==0 the MSB of key[0]) selects whether the accumulator is the left or
 * right input at level b. Returns 1 if the proof reproduces `root`, 0 otherwise. */
int vault_witness_merkle_verify(const uint8_t key[8], const uint8_t leaf_hash[32],
                                const uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32],
                                const uint8_t root[32]);

/* Ceiling on occupied leaves per checkpoint (2^20). Above it, callers raise a
 * typed checkpoint_shard_ceiling_exceeded rather than signing. */
#define VAULT_WITNESS_SHARD_CEILING (1u << 20)

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_VAULT_WITNESS_MERKLE_H */
