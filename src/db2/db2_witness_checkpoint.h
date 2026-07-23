#ifndef AIMEE_DB2_WITNESS_CHECKPOINT_H
#define AIMEE_DB2_WITNESS_CHECKPOINT_H

#include <stddef.h>
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

/* Boot anchor coverage: count retained checkpoints whose signer_key_id is NOT the
 * supplied (currently derivable) key id, and hex-render one offending id into
 * `sample` for the operator's error message.
 *
 * A key-holding kb that holds a checkpoint it cannot attribute to a key it can
 * verify with has evidence it cannot check, which is the state the umbrella says
 * it must never start in. There is no historical-anchor file yet because nothing
 * rotates the server KEK, so any foreign key id today means either a restored
 * database from another install or tampering — both of which must stop startup
 * rather than be silently tolerated. When KEK rotation lands it brings the
 * historical anchor set with it and this check widens to consult it.
 *
 * Returns 0 on success (with *out_unknown set, possibly 0), -1 if the check could
 * not be performed at all — which the caller must also treat as fail-closed, since
 * "could not verify" is not "verified". */
int db2_witness_checkpoint_anchor_coverage(const uint8_t *key_id, size_t key_id_len,
                                           int64_t *out_unknown, char *sample, size_t sample_cap);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_WITNESS_CHECKPOINT_H */
