#include "modules/vault/vault_witness_verify.h"

#include <assert.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modules/vault/vault_witness_checkpoint.h"
#include "modules/vault/vault_witness_merkle.h"
#include "modules/vault/vault_witness_record.h"

/* Build a correctly linked run of `n` records for shard (tenant,provider), each
 * carrying the digest of its predecessor as witness_pred_hash. */
static void build_chain(vault_witness_record_t *out, size_t n, const char *tenant,
                        const char *provider)
{
   uint8_t prev[32];
   for (size_t i = 0; i < n; i++)
   {
      vault_witness_record_t *r = &out[i];
      memset(r, 0, sizeof *r);
      r->source = VAULT_WITNESS_SRC_REWRAP;
      r->has_source_pred = 0;
      r->shard_seq = i + 1;
      r->is_first_in_shard = (i == 0);
      r->seal_epoch = 3;
      r->fencing_token = 4;
      memset(r->source_hash, (int)(0x20 + i), 32);
      snprintf(r->source_id, sizeof r->source_id, "s%zu", i);
      snprintf(r->tenant, sizeof r->tenant, "%s", tenant);
      snprintf(r->provider, sizeof r->provider, "%s", provider);
      snprintf(r->timestamp, sizeof r->timestamp, "2026-07-23T00:00:%02zuZ", i % 60);
      snprintf(r->group_id, sizeof r->group_id, "op");
      if (i == 0)
         assert(vault_witness_genesis_sentinel(tenant, provider, r->witness_pred_hash) == 0);
      else
         memcpy(r->witness_pred_hash, prev, 32);
      assert(vault_witness_record_digest(r, prev) == 0);
   }
}

static void test_chain_ok(void)
{
   vault_witness_record_t recs[8];
   build_chain(recs, 8, "!kb", "!reseal");
   size_t brk = 999;
   assert(vault_witness_verify_chain(recs, 8, &brk) == VAULT_WITNESS_CHAIN_OK);
}

static void test_chain_empty(void)
{
   assert(vault_witness_verify_chain(NULL, 0, NULL) == VAULT_WITNESS_CHAIN_EMPTY);
}

static void test_chain_bad_genesis(void)
{
   vault_witness_record_t recs[3];
   build_chain(recs, 3, "!kb", "!open");
   /* Start the run at index 1 (a non-first record) -> bad genesis. */
   size_t brk = 999;
   assert(vault_witness_verify_chain(&recs[1], 2, &brk) == VAULT_WITNESS_CHAIN_BAD_GENESIS);
   assert(brk == 0);
}

static void test_chain_broken_link(void)
{
   vault_witness_record_t recs[5];
   build_chain(recs, 5, "!kb", "!audit");
   /* Corrupt record 3's witness predecessor: the link from 2->3 breaks. */
   recs[3].witness_pred_hash[0] ^= 0xFF;
   size_t brk = 999;
   assert(vault_witness_verify_chain(recs, 5, &brk) == VAULT_WITNESS_CHAIN_BROKEN_LINK);
   assert(brk == 3);
}

static void test_chain_seq_gap(void)
{
   vault_witness_record_t recs[4];
   build_chain(recs, 4, "!kb", "!audit");
   recs[2].shard_seq = 99; /* not prev+1 */
   /* The digest changes with shard_seq, but the seq-gap check fires before the
    * link check. Rebuild the record's digest-consistency is irrelevant here:
    * record_valid still passes (seq>0), so we reach the seq check. */
   size_t brk = 999;
   assert(vault_witness_verify_chain(recs, 4, &brk) == VAULT_WITNESS_CHAIN_SEQ_GAP);
   assert(brk == 2);
}

static void test_chain_shard_mismatch(void)
{
   vault_witness_record_t recs[4];
   build_chain(recs, 4, "!kb", "!audit");
   snprintf(recs[2].provider, sizeof recs[2].provider, "!other");
   size_t brk = 999;
   assert(vault_witness_verify_chain(recs, 4, &brk) == VAULT_WITNESS_CHAIN_SHARD_MISMATCH);
   assert(brk == 2);
}

/* --- checkpoint run continuity --- */

static void gen_keypair(uint8_t priv[32], uint8_t pub[32])
{
   EVP_PKEY *pkey = NULL;
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &pkey) == 1);
   size_t l = 32;
   assert(EVP_PKEY_get_raw_private_key(pkey, priv, &l) == 1);
   l = 32;
   assert(EVP_PKEY_get_raw_public_key(pkey, pub, &l) == 1);
   EVP_PKEY_free(pkey);
   EVP_PKEY_CTX_free(ctx);
}

/* Build `n` checkpoints where each links to the prior via predecessor_digest. */
static void build_checkpoint_run(vault_witness_checkpoint_t *out, size_t n, const uint8_t priv[32])
{
   uint8_t prev[32];
   for (size_t i = 0; i < n; i++)
   {
      vault_witness_checkpoint_t *cp = &out[i];
      memset(cp, 0, sizeof *cp);
      cp->version = 1;
      cp->seq = i + 1;
      cp->sig_alg = VAULT_WITNESS_SIG_ED25519;
      cp->sig_version = 1;
      cp->shard_count = 3;
      memset(cp->root, (int)(0x40 + i), 32);
      memset(cp->leaf_snapshot_digest, (int)(0x60 + i), 32);
      memset(cp->signer_key_id, 0xA0, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
      snprintf(cp->created_at, sizeof cp->created_at, "2026-07-23T00:%02zu:00Z", i % 60);
      if (i == 0)
      {
         cp->has_predecessor = 0;
      }
      else
      {
         cp->has_predecessor = 1;
         memcpy(cp->predecessor_digest, prev, 32);
      }
      assert(vault_witness_checkpoint_sign_ed25519(cp, priv) == 0);
      assert(vault_witness_checkpoint_digest(cp, prev) == 0);
   }
}

static void test_checkpoint_run_ok(void)
{
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   vault_witness_checkpoint_t cps[5];
   build_checkpoint_run(cps, 5, priv);
   size_t gap = 999;
   assert(vault_witness_verify_checkpoint_run(cps, 5, &gap) == VAULT_WITNESS_CONTINUITY_OK);
}

static void test_checkpoint_run_gap_unproven(void)
{
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   vault_witness_checkpoint_t cps[5];
   build_checkpoint_run(cps, 5, priv);
   /* Drop checkpoint index 2 (simulate a suppressed/missing checkpoint): the
    * consumer holds 0,1,3,4. cps[3].predecessor points at the missing cps[2], so
    * linking 1->3 mismatches -> UNPROVEN, a work item, with the gap surfaced. */
   vault_witness_checkpoint_t held[4] = {cps[0], cps[1], cps[3], cps[4]};
   size_t gap = 999;
   assert(vault_witness_verify_checkpoint_run(held, 4, &gap) == VAULT_WITNESS_CONTINUITY_UNPROVEN);
   assert(gap == 1); /* the gap is after held index 1 */
}

static void test_checkpoint_run_single(void)
{
   uint8_t priv[32], pub[32];
   gen_keypair(priv, pub);
   vault_witness_checkpoint_t cps[1];
   build_checkpoint_run(cps, 1, priv);
   assert(vault_witness_verify_checkpoint_run(cps, 1, NULL) == VAULT_WITNESS_CONTINUITY_OK);
}

/* Build a real SMT over a few shard identities, then prove one is included and a
 * tampered head is not. This exercises the identity->key/leaf recomputation. */
static void test_inclusion(void)
{
   struct
   {
      const char *tenant, *provider;
      uint64_t seq;
      uint8_t head[32];
   } shards[3] = {{"!kb", "!audit", 12, {0}}, {"!kb", "!reseal", 4, {0}}, {"!kb", "!open", 7, {0}}};
   for (int i = 0; i < 3; i++)
      memset(shards[i].head, 0x11 * (i + 1), 32);

   vault_witness_leaf_t leaves[3];
   for (int i = 0; i < 3; i++)
   {
      assert(vault_witness_shard_key_hash(shards[i].tenant, shards[i].provider, leaves[i].key) == 0);
      assert(vault_witness_leaf_hash(shards[i].tenant, shards[i].provider, shards[i].seq,
                                     shards[i].head, leaves[i].hash) == 0);
   }
   /* sort leaves by key for the SMT contract */
   for (int a = 0; a < 3; a++)
      for (int b = a + 1; b < 3; b++)
         if (memcmp(leaves[a].key, leaves[b].key, 8) > 0)
         {
            vault_witness_leaf_t t = leaves[a];
            leaves[a] = leaves[b];
            leaves[b] = t;
         }
   uint8_t root[32];
   assert(vault_witness_merkle_root(leaves, 3, root) == 0);

   /* Prove the audit shard's inclusion. Find its index post-sort. */
   uint8_t akey[8], aleaf[32];
   assert(vault_witness_shard_key_hash("!kb", "!audit", akey) == 0);
   assert(vault_witness_leaf_hash("!kb", "!audit", 12, shards[0].head, aleaf) == 0);
   size_t idx = 4;
   for (size_t i = 0; i < 3; i++)
      if (memcmp(leaves[i].key, akey, 8) == 0)
         idx = i;
   assert(idx < 3);
   uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32];
   assert(vault_witness_merkle_proof(leaves, 3, idx, proof) == 0);

   assert(vault_witness_verify_inclusion("!kb", "!audit", 12, shards[0].head, proof, root) == 1);
   /* A tampered head at the same position must not verify. */
   uint8_t bad_head[32];
   memcpy(bad_head, shards[0].head, 32);
   bad_head[0] ^= 0xFF;
   assert(vault_witness_verify_inclusion("!kb", "!audit", 12, bad_head, proof, root) == 0);
   /* A different shard identity with the same proof must not verify. */
   assert(vault_witness_verify_inclusion("!kb", "!open", 12, shards[0].head, proof, root) == 0);
}

int main(void)
{
   test_chain_ok();
   test_chain_empty();
   test_chain_bad_genesis();
   test_chain_broken_link();
   test_chain_seq_gap();
   test_chain_shard_mismatch();
   test_checkpoint_run_ok();
   test_checkpoint_run_gap_unproven();
   test_checkpoint_run_single();
   test_inclusion();
   printf("test_vault_witness_verify: all passed\n");
   return 0;
}
