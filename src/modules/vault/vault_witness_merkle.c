#include "vault_witness_merkle.h"

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <string.h>

static void put_u32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24);
   p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);
   p[3] = (uint8_t)v;
}

static void put_u64(uint8_t *p, uint64_t v)
{
   for (unsigned i = 0; i < 8; i++)
      p[i] = (uint8_t)(v >> (56U - 8U * i));
}

/* Bit i of the 64-bit key, i==0 the MSB of key[0]. */
static int key_bit(const uint8_t key[8], unsigned i)
{
   return (key[i / 8] >> (7 - (i % 8))) & 1;
}

/* node hash = SHA-256(NODE_LABEL || left || right). */
static void node_hash(const uint8_t left[32], const uint8_t right[32], uint8_t out[32])
{
   size_t label_len = strlen(VAULT_WITNESS_SMT_NODE_LABEL);
   uint8_t buf[sizeof(VAULT_WITNESS_SMT_NODE_LABEL) + 64];
   memcpy(buf, VAULT_WITNESS_SMT_NODE_LABEL, label_len);
   memcpy(buf + label_len, left, 32);
   memcpy(buf + label_len + 32, right, 32);
   SHA256(buf, label_len + 64, out);
}

/* empty[0] = SHA-256(EMPTY_LABEL); empty[l] = node(empty[l-1], empty[l-1]).
 * `empty` must hold DEPTH+1 entries. */
static void compute_empty(uint8_t empty[VAULT_WITNESS_SMT_DEPTH + 1][32])
{
   SHA256(( const unsigned char *)VAULT_WITNESS_SMT_EMPTY_LABEL,
          strlen(VAULT_WITNESS_SMT_EMPTY_LABEL), empty[0]);
   for (unsigned l = 1; l <= VAULT_WITNESS_SMT_DEPTH; l++)
      node_hash(empty[l - 1], empty[l - 1], empty[l]);
}

/* Sorted, strictly ascending, and no duplicate key. */
static int leaves_ok(const vault_witness_leaf_t *leaves, size_t n)
{
   for (size_t i = 1; i < n; i++)
      if (memcmp(leaves[i - 1].key, leaves[i].key, 8) >= 0)
         return 0;
   return 1;
}

/* Recursively compute the subtree root over the sorted leaf slice [lo,hi) at a
 * node with `level` bits remaining (root has level==DEPTH). If `target` is
 * non-NULL it lies within this slice; the sibling subtree root at each level on
 * the path to it is written into proof[disc_bit]. */
static void root_rec(const vault_witness_leaf_t *leaves, size_t lo, size_t hi, unsigned level,
                     const uint8_t empty[VAULT_WITNESS_SMT_DEPTH + 1][32], const uint8_t *target,
                     uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32], uint8_t out[32])
{
   if (lo == hi)
   {
      memcpy(out, empty[level], 32);
      return;
   }
   if (level == 0)
   {
      /* Exactly one leaf occupies a fully-resolved 64-bit position. */
      memcpy(out, leaves[lo].hash, 32);
      return;
   }
   unsigned disc = VAULT_WITNESS_SMT_DEPTH - level; /* bit index, 0==MSB */
   /* Sorted by key, so the bit-0 group is a prefix; find the split. */
   size_t split = lo;
   while (split < hi && key_bit(leaves[split].key, disc) == 0)
      split++;

   uint8_t lroot[32], rroot[32];
   const uint8_t *ltarget = NULL, *rtarget = NULL;
   if (target)
   {
      if (key_bit(target, disc) == 0)
         ltarget = target;
      else
         rtarget = target;
   }
   root_rec(leaves, lo, split, level - 1, empty, ltarget, proof, lroot);
   root_rec(leaves, split, hi, level - 1, empty, rtarget, proof, rroot);
   if (target)
      memcpy(proof[disc], target == ltarget ? rroot : lroot, 32);
   node_hash(lroot, rroot, out);
}

int vault_witness_leaf_hash(const char *tenant, const char *provider, uint64_t sequence,
                            const uint8_t head_hash[32], uint8_t out[32])
{
   if (!out)
      return -1;
   OPENSSL_cleanse(out, 32);
   if (!tenant || !provider || !head_hash)
      return -1;
   size_t tl = strlen(tenant), pl = strlen(provider);
   /* Bounded to the record's shard-key caps (128 each); the SMT never sees longer
    * keys — vault_witness_shard_key_hash and _genesis_sentinel enforce the same
    * 128 cap, so keeping them aligned avoids a latent accept-here/reject-there. */
   if (tl == 0 || pl == 0 || tl > 128 || pl > 128)
      return -1;
   size_t label_len = strlen(VAULT_WITNESS_SMT_LEAF_LABEL);
   uint8_t buf[sizeof(VAULT_WITNESS_SMT_LEAF_LABEL) + 4 + 128 + 4 + 128 + 8 + 32];
   size_t off = 0;
   memcpy(buf, VAULT_WITNESS_SMT_LEAF_LABEL, label_len);
   off = label_len;
   put_u32(buf + off, (uint32_t)tl);
   off += 4;
   memcpy(buf + off, tenant, tl);
   off += tl;
   put_u32(buf + off, (uint32_t)pl);
   off += 4;
   memcpy(buf + off, provider, pl);
   off += pl;
   put_u64(buf + off, sequence);
   off += 8;
   memcpy(buf + off, head_hash, 32);
   off += 32;
   SHA256(buf, off, out);
   return 0;
}

int vault_witness_merkle_root(const vault_witness_leaf_t *leaves, size_t n, uint8_t root[32])
{
   if (!root)
      return -1;
   OPENSSL_cleanse(root, 32);
   if (n > VAULT_WITNESS_SHARD_CEILING || (n && !leaves) || !leaves_ok(leaves, n))
      return -1;
   uint8_t empty[VAULT_WITNESS_SMT_DEPTH + 1][32];
   compute_empty(empty);
   root_rec(leaves, 0, n, VAULT_WITNESS_SMT_DEPTH, empty, NULL, NULL, root);
   return 0;
}

int vault_witness_merkle_proof(const vault_witness_leaf_t *leaves, size_t n, size_t index,
                               uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32])
{
   if (!proof)
      return -1;
   memset(proof, 0, sizeof(uint8_t[VAULT_WITNESS_SMT_DEPTH][32]));
   if (!leaves || index >= n || n > VAULT_WITNESS_SHARD_CEILING || !leaves_ok(leaves, n))
      return -1;
   uint8_t empty[VAULT_WITNESS_SMT_DEPTH + 1][32];
   compute_empty(empty);
   uint8_t root[32];
   root_rec(leaves, 0, n, VAULT_WITNESS_SMT_DEPTH, empty, leaves[index].key, proof, root);
   OPENSSL_cleanse(root, 32);
   return 0;
}

int vault_witness_merkle_verify(const uint8_t key[8], const uint8_t leaf_hash[32],
                                const uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32],
                                const uint8_t root[32])
{
   if (!key || !leaf_hash || !proof || !root)
      return 0;
   uint8_t acc[32];
   memcpy(acc, leaf_hash, 32);
   for (int b = VAULT_WITNESS_SMT_DEPTH - 1; b >= 0; b--)
   {
      if (key_bit(key, (unsigned)b) == 0)
         node_hash(acc, proof[b], acc);
      else
         node_hash(proof[b], acc, acc);
   }
   return CRYPTO_memcmp(acc, root, 32) == 0;
}
