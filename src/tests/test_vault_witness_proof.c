#include "modules/vault/vault_witness_proof.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/vault/vault_witness_merkle.h"

static int leaf_cmp(const void *a, const void *b)
{
   return memcmp(((const vault_witness_leaf_t *)a)->key, ((const vault_witness_leaf_t *)b)->key, 8);
}

/* Build a real SMT over three shards, emit a proof for one, and check encode ->
 * decode -> verify against the checkpoint root. */
static void test_proof_roundtrip_and_verify(void)
{
   const char *names[3][2] = {{"!kb", "!audit"}, {"!kb", "!reseal"}, {"!kb", "!open"}};
   uint64_t seqs[3] = {12, 4, 7};
   uint8_t heads[3][32];
   for (int i = 0; i < 3; i++)
      memset(heads[i], 0x11 * (i + 1), 32);

   vault_witness_leaf_t leaves[3];
   for (int i = 0; i < 3; i++)
   {
      assert(vault_witness_shard_key_hash(names[i][0], names[i][1], leaves[i].key) == 0);
      assert(vault_witness_leaf_hash(names[i][0], names[i][1], seqs[i], heads[i], leaves[i].hash) ==
             0);
   }
   qsort(leaves, 3, sizeof leaves[0], leaf_cmp);
   uint8_t root[32];
   assert(vault_witness_merkle_root(leaves, 3, root) == 0);

   /* Proof for the audit shard. */
   uint8_t akey[8];
   assert(vault_witness_shard_key_hash("!kb", "!audit", akey) == 0);
   size_t idx = 4;
   for (size_t i = 0; i < 3; i++)
      if (memcmp(leaves[i].key, akey, 8) == 0)
         idx = i;
   assert(idx < 3);

   vault_witness_proof_t p;
   memset(&p, 0, sizeof p);
   p.checkpoint_seq = 9;
   snprintf(p.tenant, sizeof p.tenant, "!kb");
   snprintf(p.provider, sizeof p.provider, "!audit");
   p.sequence = 12;
   memcpy(p.head_hash, heads[0], 32);
   assert(vault_witness_merkle_proof(leaves, 3, idx, p.path) == 0);

   assert(vault_witness_proof_verify(&p, root) == 1);

   /* Wire round-trip. */
   uint8_t wire[VAULT_WITNESS_PROOF_WIRE_MAX];
   size_t len = 0;
   assert(vault_witness_proof_encode(&p, wire, sizeof wire, &len) == 0);
   vault_witness_proof_t back;
   assert(vault_witness_proof_decode(wire, len, &back) == 0);
   assert(back.checkpoint_seq == 9 && back.sequence == 12);
   assert(strcmp(back.tenant, "!kb") == 0 && strcmp(back.provider, "!audit") == 0);
   assert(vault_witness_proof_verify(&back, root) == 1);

   /* Tampered head does not verify; a different shard identity does not verify. */
   vault_witness_proof_t t = p;
   t.head_hash[0] ^= 0xFF;
   assert(vault_witness_proof_verify(&t, root) == 0);
   vault_witness_proof_t t2 = p;
   snprintf(t2.provider, sizeof t2.provider, "!open");
   assert(vault_witness_proof_verify(&t2, root) == 0);
}

static void test_decode_rejections(void)
{
   vault_witness_proof_t p;
   memset(&p, 0, sizeof p);
   p.checkpoint_seq = 1;
   snprintf(p.tenant, sizeof p.tenant, "t");
   snprintf(p.provider, sizeof p.provider, "pr");
   p.sequence = 1;
   uint8_t wire[VAULT_WITNESS_PROOF_WIRE_MAX];
   size_t len = 0;
   assert(vault_witness_proof_encode(&p, wire, sizeof wire, &len) == 0);
   vault_witness_proof_t out;
   assert(vault_witness_proof_decode(wire, len - 1, &out) == -1); /* truncated */
   assert(vault_witness_proof_decode(wire, len + 1, &out) == -1); /* trailing */
   uint8_t bad[VAULT_WITNESS_PROOF_WIRE_MAX];
   memcpy(bad, wire, len);
   bad[0] ^= 0xFF; /* magic */
   assert(vault_witness_proof_decode(bad, len, &out) == -1);
}

int main(void)
{
   test_proof_roundtrip_and_verify();
   test_decode_rejections();
   printf("test_vault_witness_proof: all passed\n");
   return 0;
}
