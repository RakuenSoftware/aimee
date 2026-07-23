#include "vault_witness_verify.h"

#include <openssl/crypto.h>
#include <string.h>

vault_witness_chain_result_t vault_witness_verify_chain(const vault_witness_record_t *records,
                                                        size_t n, size_t *break_index)
{
   if (break_index)
      *break_index = 0;
   if (n == 0 || !records)
      return VAULT_WITNESS_CHAIN_EMPTY;

   uint8_t prev_digest[32];
   for (size_t i = 0; i < n; i++)
   {
      const vault_witness_record_t *r = &records[i];

      /* Structural validity + a computable digest. The digest call re-runs
       * record_valid internally, so a malformed record is rejected here. */
      uint8_t digest[32];
      if (vault_witness_record_digest(r, digest) != 0)
      {
         if (break_index)
            *break_index = i;
         return VAULT_WITNESS_CHAIN_BAD_RECORD;
      }

      if (i == 0)
      {
         /* First supplied record must genuinely be first-in-shard: seq 1 and its
          * witness predecessor equal to the shard-bound genesis sentinel.
          * record_valid already enforces the is_first/seq/genesis equivalence, so
          * checking is_first_in_shard is sufficient and precise. */
         if (!r->is_first_in_shard)
         {
            if (break_index)
               *break_index = 0;
            return VAULT_WITNESS_CHAIN_BAD_GENESIS;
         }
      }
      else
      {
         const vault_witness_record_t *p = &records[i - 1];
         if (strcmp(r->tenant, p->tenant) != 0 || strcmp(r->provider, p->provider) != 0)
         {
            if (break_index)
               *break_index = i;
            return VAULT_WITNESS_CHAIN_SHARD_MISMATCH;
         }
         if (r->shard_seq != p->shard_seq + 1)
         {
            if (break_index)
               *break_index = i;
            return VAULT_WITNESS_CHAIN_SEQ_GAP;
         }
         if (CRYPTO_memcmp(r->witness_pred_hash, prev_digest, 32) != 0)
         {
            if (break_index)
               *break_index = i;
            return VAULT_WITNESS_CHAIN_BROKEN_LINK;
         }
      }
      memcpy(prev_digest, digest, 32);
   }
   OPENSSL_cleanse(prev_digest, sizeof prev_digest);
   return VAULT_WITNESS_CHAIN_OK;
}

int vault_witness_verify_inclusion(const char *tenant, const char *provider, uint64_t sequence,
                                   const uint8_t head_hash[32],
                                   const uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32],
                                   const uint8_t root[32])
{
   if (!tenant || !provider || !head_hash || !proof || !root)
      return 0;
   uint8_t key[8], leaf[32];
   /* Recompute both the position key and the leaf commitment from the identity,
    * so a caller cannot substitute one shard's leaf at another's position. */
   if (vault_witness_shard_key_hash(tenant, provider, key) != 0 ||
       vault_witness_leaf_hash(tenant, provider, sequence, head_hash, leaf) != 0)
      return 0;
   return vault_witness_merkle_verify(key, leaf, proof, root);
}

vault_witness_continuity_t vault_witness_verify_checkpoint_run(
    const vault_witness_checkpoint_t *checkpoints, size_t n, size_t *gap_after_index)
{
   if (gap_after_index)
      *gap_after_index = 0;
   if (n <= 1 || !checkpoints)
      return VAULT_WITNESS_CONTINUITY_OK; /* zero or one checkpoint links to nothing */

   /* From emitted bytes alone a missing intermediate checkpoint and a malicious
    * fork are indistinguishable: both show as "this checkpoint's predecessor
    * digest is not the one we hold." So a linkage mismatch is UNPROVEN — a work
    * item for the operator to resolve by comparing cross-gap leaf sets — never a
    * silent pass. BROKEN is reserved for the structurally impossible case: a
    * mid-run checkpoint that denies having a predecessor at all. */
   int saw_gap = 0, gap_recorded = 0;
   for (size_t i = 1; i < n; i++)
   {
      uint8_t prev_digest[32];
      if (vault_witness_checkpoint_digest(&checkpoints[i - 1], prev_digest) != 0)
      {
         if (gap_after_index)
            *gap_after_index = i - 1;
         return VAULT_WITNESS_CONTINUITY_BROKEN;
      }
      vault_witness_continuity_t c =
          vault_witness_checkpoint_continuity(&checkpoints[i], prev_digest);
      OPENSSL_cleanse(prev_digest, sizeof prev_digest);

      if (c != VAULT_WITNESS_CONTINUITY_OK)
      {
         if (!checkpoints[i].has_predecessor)
         {
            if (gap_after_index)
               *gap_after_index = i - 1;
            return VAULT_WITNESS_CONTINUITY_BROKEN;
         }
         saw_gap = 1;
         if (gap_after_index && !gap_recorded)
         {
            *gap_after_index = i - 1;
            gap_recorded = 1;
         }
      }
   }
   return saw_gap ? VAULT_WITNESS_CONTINUITY_UNPROVEN : VAULT_WITNESS_CONTINUITY_OK;
}
