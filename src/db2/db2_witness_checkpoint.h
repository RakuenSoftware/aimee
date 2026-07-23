#ifndef AIMEE_DB2_WITNESS_CHECKPOINT_H
#define AIMEE_DB2_WITNESS_CHECKPOINT_H

#include <stdint.h>

/* P7-witness-e2: the checkpoint producer. Ties the SQL surface (leaf scan +
 * cross-check + fenced persist) to the C surface (depth-64 SMT root + vault-held
 * Ed25519 sign) in one REPEATABLE READ transaction so the signed root matches the
 * persisted leaf set. E2 cadence calls this; E1/E2 have no other caller.
 *
 * Flow (architect review 11384): BEGIN REPEATABLE READ -> read control fence ->
 * org_vault_witness_checkpoint_leaves() (raises on head_log_mismatch / ceiling) ->
 * C builds the SMT root over the verified leaves and the leaf snapshot ->
 * reconstruct the previous checkpoint's digest as predecessor -> vault-held sign
 * -> org_vault_witness_checkpoint_persist() (fenced, monotonic) -> COMMIT.
 */

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
   DB2_WITNESS_CP_OK = 0,       /* a checkpoint was signed and persisted */
   DB2_WITNESS_CP_EMPTY,        /* no non-empty shards: nothing to checkpoint (no-op) */
   DB2_WITNESS_CP_TRANSIENT,    /* no connection / retryable serialization failure */
   DB2_WITNESS_CP_HEAD_MISMATCH,/* head_log_mismatch (P7W01): a shard head diverged */
   DB2_WITNESS_CP_CEILING,      /* checkpoint_shard_ceiling_exceeded (P7W02) */
   DB2_WITNESS_CP_FENCE_STALE,  /* control fence advanced under us (P7W03) */
   DB2_WITNESS_CP_ERROR         /* any other failure */
} db2_witness_checkpoint_result_t;

/* Produce one checkpoint. On DB2_WITNESS_CP_OK, *out_seq (if non-NULL) receives the
 * new checkpoint sequence. The typed non-OK results map the SQL/producer failure
 * modes so the cadence can raise the right integrity alert. This function commits
 * or rolls back its own transaction. */
db2_witness_checkpoint_result_t db2_witness_checkpoint_produce(int64_t *out_seq);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_WITNESS_CHECKPOINT_H */
