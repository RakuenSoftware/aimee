#ifndef AIMEE_TEST_VAULT_WITNESS_PROVIDER_FIXTURE_H
#define AIMEE_TEST_VAULT_WITNESS_PROVIDER_FIXTURE_H

#include "modules/db2/c/db2_vault_witness_provider.h"
#include "modules/vault/vault_witness_export.h"
#include "modules/vault/vault_witness_signer.h"
#include "modules/vault/vault_witness_verify.h"

static int test_vault_witness_checkpoint_verify(const vault_witness_checkpoint_t *checkpoint,
                                                const vault_witness_anchor_t *anchors,
                                                size_t anchor_count)
{
   return (int)vault_witness_checkpoint_verify(checkpoint, anchors, anchor_count);
}

static int test_vault_witness_export_frame(int kind, const uint8_t *payload, size_t payload_len,
                                           uint8_t *out, size_t cap, size_t *out_len)
{
   if (kind < VAULT_WITNESS_EXPORT_RECORD || kind > VAULT_WITNESS_EXPORT_SNAPSHOT)
      return -1;
   return vault_witness_export_frame((vault_witness_export_kind_t)kind, payload, payload_len, out,
                                     cap, out_len);
}

static int test_vault_witness_verify_checkpoint_run(const vault_witness_checkpoint_t *checkpoints,
                                                    size_t count, size_t *gap_after_index)
{
   return (int)vault_witness_verify_checkpoint_run(checkpoints, count, gap_after_index);
}

static inline void test_register_vault_witness_provider(void)
{
   static const db2_vault_witness_provider_t provider = {
       .checkpoint_digest = vault_witness_checkpoint_digest,
       .checkpoint_encode = vault_witness_checkpoint_encode,
       .checkpoint_sign = vault_witness_checkpoint_sign,
       .checkpoint_verify = test_vault_witness_checkpoint_verify,
       .export_frame = test_vault_witness_export_frame,
       .leaf_hash = vault_witness_leaf_hash,
       .merkle_root = vault_witness_merkle_root,
       .record_digest = vault_witness_record_digest,
       .record_encode = vault_witness_record_encode,
       .shard_key_hash = vault_witness_shard_key_hash,
       .signer_identity = vault_witness_signer_identity,
       .verify_checkpoint_run = test_vault_witness_verify_checkpoint_run,
   };
   aimee_db2_register_vault_witness_provider(&provider);
}

#endif /* AIMEE_TEST_VAULT_WITNESS_PROVIDER_FIXTURE_H */
