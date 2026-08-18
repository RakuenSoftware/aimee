#ifndef AIMEE_DB2_VAULT_WITNESS_PROVIDER_H
#define AIMEE_DB2_VAULT_WITNESS_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include "modules/vault/vault_witness_checkpoint.h"
#include "modules/vault/vault_witness_merkle.h"
#include "modules/vault/vault_witness_record.h"

typedef struct
{
   int (*checkpoint_digest)(const vault_witness_checkpoint_t *checkpoint, uint8_t digest[32]);
   int (*checkpoint_encode)(const vault_witness_checkpoint_t *checkpoint, uint8_t *out, size_t cap,
                            size_t *out_len);
   int (*checkpoint_sign)(vault_witness_checkpoint_t *checkpoint);
   int (*checkpoint_verify)(const vault_witness_checkpoint_t *checkpoint,
                            const vault_witness_anchor_t *anchors, size_t anchor_count);
   int (*export_frame)(int kind, const uint8_t *payload, size_t payload_len, uint8_t *out,
                       size_t cap, size_t *out_len);
   int (*leaf_hash)(const char *tenant, const char *provider, uint64_t sequence,
                    const uint8_t head_hash[32], uint8_t out[32]);
   int (*merkle_root)(const vault_witness_leaf_t *leaves, size_t count, uint8_t root[32]);
   int (*record_digest)(const vault_witness_record_t *record, uint8_t digest[32]);
   int (*record_encode)(const vault_witness_record_t *record, uint8_t *out, size_t cap,
                        size_t *out_len);
   int (*shard_key_hash)(const char *tenant, const char *provider, uint8_t out[8]);
   int (*signer_identity)(uint8_t public_key[32], uint8_t key_id[16]);
   int (*verify_checkpoint_run)(const vault_witness_checkpoint_t *checkpoints, size_t count,
                                size_t *gap_after_index);
} db2_vault_witness_provider_t;

void aimee_db2_register_vault_witness_provider(const db2_vault_witness_provider_t *provider);

int db2_vault_witness_checkpoint_digest(const vault_witness_checkpoint_t *checkpoint,
                                        uint8_t digest[32]);
int db2_vault_witness_checkpoint_encode(const vault_witness_checkpoint_t *checkpoint, uint8_t *out,
                                        size_t cap, size_t *out_len);
int db2_vault_witness_checkpoint_sign(vault_witness_checkpoint_t *checkpoint);
int db2_vault_witness_checkpoint_verify(const vault_witness_checkpoint_t *checkpoint,
                                        const vault_witness_anchor_t *anchors, size_t anchor_count);
int db2_vault_witness_export_frame(int kind, const uint8_t *payload, size_t payload_len,
                                   uint8_t *out, size_t cap, size_t *out_len);
int db2_vault_witness_leaf_hash(const char *tenant, const char *provider, uint64_t sequence,
                                const uint8_t head_hash[32], uint8_t out[32]);
int db2_vault_witness_merkle_root(const vault_witness_leaf_t *leaves, size_t count,
                                  uint8_t root[32]);
int db2_vault_witness_record_digest(const vault_witness_record_t *record, uint8_t digest[32]);
int db2_vault_witness_record_encode(const vault_witness_record_t *record, uint8_t *out, size_t cap,
                                    size_t *out_len);
int db2_vault_witness_shard_key_hash(const char *tenant, const char *provider, uint8_t out[8]);
int db2_vault_witness_signer_identity(uint8_t public_key[32], uint8_t key_id[16]);
int db2_vault_witness_verify_checkpoint_run(const vault_witness_checkpoint_t *checkpoints,
                                            size_t count, size_t *gap_after_index);

#endif /* AIMEE_DB2_VAULT_WITNESS_PROVIDER_H */
