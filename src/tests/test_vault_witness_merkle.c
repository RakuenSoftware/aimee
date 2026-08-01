#include "modules/vault/vault_witness_merkle.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int leaf_cmp(const void *a, const void *b)
{
   return memcmp(((const vault_witness_leaf_t *)a)->key, ((const vault_witness_leaf_t *)b)->key, 8);
}

/* Deterministic leaf set of `n` distinct keys. */
static size_t make_leaves(vault_witness_leaf_t *out, size_t n, uint8_t salt)
{
   for (size_t i = 0; i < n; i++)
   {
      for (int k = 0; k < 8; k++)
         out[i].key[k] = (uint8_t)((i * 131 + k * 17 + salt * 7) & 0xFF);
      for (int h = 0; h < 32; h++)
         out[i].hash[h] = (uint8_t)((i * 7 + h * 3 + salt) & 0xFF);
   }
   qsort(out, n, sizeof out[0], leaf_cmp);
   return n;
}

static void test_root_and_proofs(void)
{
   vault_witness_leaf_t leaves[64];
   size_t n = make_leaves(leaves, 40, 1);
   uint8_t root[32];
   assert(vault_witness_merkle_root(leaves, n, root) == 0);

   for (size_t i = 0; i < n; i++)
   {
      uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32];
      assert(vault_witness_merkle_proof(leaves, n, i, proof) == 0);
      assert(vault_witness_merkle_verify(leaves[i].key, leaves[i].hash, proof, root) == 1);

      /* A proof from one checkpoint does not verify against another's root. */
      uint8_t other_root[32];
      vault_witness_leaf_t leaves2[64];
      size_t n2 = make_leaves(leaves2, 40, 2);
      assert(vault_witness_merkle_root(leaves2, n2, other_root) == 0);
      assert(vault_witness_merkle_verify(leaves[i].key, leaves[i].hash, proof, other_root) == 0);

      /* Two transposed levels break the proof. */
      uint8_t bad[VAULT_WITNESS_SMT_DEPTH][32];
      memcpy(bad, proof, sizeof bad);
      uint8_t tmp[32];
      memcpy(tmp, bad[10], 32);
      memcpy(bad[10], bad[40], 32);
      memcpy(bad[40], tmp, 32);
      if (memcmp(bad[10], proof[10], 32) != 0) /* only meaningful if the siblings differ */
         assert(vault_witness_merkle_verify(leaves[i].key, leaves[i].hash, bad, root) == 0);
   }
}

static void test_absent_key_fails(void)
{
   vault_witness_leaf_t leaves[32];
   size_t n = make_leaves(leaves, 20, 3);
   uint8_t root[32];
   assert(vault_witness_merkle_root(leaves, n, root) == 0);

   /* A key not in the set, with a fabricated proof of all-empty siblings, must not
    * verify against the real root. */
   uint8_t absent[8];
   memset(absent, 0xFE, 8);
   for (size_t i = 0; i < n; i++)
      assert(memcmp(leaves[i].key, absent, 8) != 0);
   uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32];
   memset(proof, 0, sizeof proof);
   uint8_t leaf_hash[32];
   memset(leaf_hash, 0xAB, 32);
   assert(vault_witness_merkle_verify(absent, leaf_hash, proof, root) == 0);
}

static void test_order_independent_root(void)
{
   vault_witness_leaf_t a[32], b[32];
   size_t n = make_leaves(a, 25, 5);
   memcpy(b, a, n * sizeof a[0]);
   /* Reverse b, then re-sort — root must match regardless of pre-sort order. */
   for (size_t i = 0; i < n / 2; i++)
   {
      vault_witness_leaf_t t = b[i];
      b[i] = b[n - 1 - i];
      b[n - 1 - i] = t;
   }
   qsort(b, n, sizeof b[0], leaf_cmp);
   uint8_t ra[32], rb[32];
   assert(vault_witness_merkle_root(a, n, ra) == 0);
   assert(vault_witness_merkle_root(b, n, rb) == 0);
   assert(memcmp(ra, rb, 32) == 0);
}

static void test_duplicate_key_rejected(void)
{
   vault_witness_leaf_t leaves[4];
   memset(leaves, 0, sizeof leaves);
   for (int i = 0; i < 4; i++)
      memset(leaves[i].hash, i, 32);
   memset(leaves[0].key, 1, 8);
   memset(leaves[1].key, 1, 8); /* collision */
   memset(leaves[2].key, 2, 8);
   memset(leaves[3].key, 3, 8);
   uint8_t root[32];
   /* Unsorted-or-duplicate is rejected. */
   assert(vault_witness_merkle_root(leaves, 4, root) == -1);
}

static void test_empty_and_single(void)
{
   uint8_t r0[32], r1[32];
   assert(vault_witness_merkle_root(NULL, 0, r0) == 0); /* all-empty root */
   vault_witness_leaf_t one;
   memset(&one, 0, sizeof one);
   memset(one.key, 0x55, 8);
   memset(one.hash, 0x77, 32);
   assert(vault_witness_merkle_root(&one, 1, r1) == 0);
   assert(memcmp(r0, r1, 32) != 0);
   uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32];
   assert(vault_witness_merkle_proof(&one, 1, 0, proof) == 0);
   assert(vault_witness_merkle_verify(one.key, one.hash, proof, r1) == 1);
}

static void test_leaf_hash_binds_shard(void)
{
   uint8_t head[32];
   memset(head, 0x22, 32);
   uint8_t h1[32], h2[32];
   assert(vault_witness_leaf_hash("acme", "anthropic", 3, head, h1) == 0);
   /* Same head + seq, different shard -> different leaf. */
   assert(vault_witness_leaf_hash("acme", "openai", 3, head, h2) == 0);
   assert(memcmp(h1, h2, 32) != 0);
   /* Boundary shift in the shard key must not collide. */
   uint8_t h3[32];
   assert(vault_witness_leaf_hash("acm", "eanthropic", 3, head, h3) == 0);
   assert(memcmp(h1, h3, 32) != 0);
}

int main(void)
{
   test_root_and_proofs();
   test_absent_key_fails();
   test_order_independent_root();
   test_duplicate_key_rejected();
   test_empty_and_single();
   test_leaf_hash_binds_shard();
   printf("test_vault_witness_merkle: all passed\n");
   return 0;
}
