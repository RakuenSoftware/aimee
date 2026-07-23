#ifndef AIMEE_VAULT_WITNESS_OFFLINE_H
#define AIMEE_VAULT_WITNESS_OFFLINE_H

#include <stddef.h>
#include <stdint.h>

#include "vault_witness_checkpoint.h"

/* P7-witness-e2: the offline verifier core. Given a captured stream of emitted
 * export frames (records, checkpoints, proofs, leaf snapshots) and an out-of-band anchor set, it
 * verifies everything reachable from bytes alone — no database. This is the
 * "detection by comparison" primitive an operator runs during an incident: it
 * reports what the retained copy proves and what, if anything, is tampered.
 *
 * The stream is a concatenation of vault_witness_export frames, each self-
 * delimiting via its header. Records are grouped by shard and their per-shard
 * chains verified; checkpoints are signature-verified against the anchors and
 * their run checked for continuity; proofs are verified against the matching
 * checkpoint's root; and each leaf snapshot is hashed and required to equal the
 * leaf_snapshot_digest inside its own signed checkpoint, so a substituted snapshot
 * cannot pass as the leaf set the signature actually committed to.
 */

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
   size_t frames, records, checkpoints, proofs, unknown_frames;
   size_t shards_ok, shards_broken;      /* per-shard record-chain results */
   size_t records_duplicate;             /* byte-identical repeats (benign re-emission) */
   size_t records_conflict;              /* SAME shard_seq, DIFFERENT record: a fork */
   size_t checkpoints_ok;                /* signature valid, key known+unrevoked */
   size_t checkpoints_bad_sig;           /* signature does not verify */
   size_t checkpoints_unknown_key;       /* signer not in anchor set */
   size_t checkpoints_revoked;           /* signer key revoked */
   size_t checkpoints_duplicate;         /* byte-identical repeats (benign re-emission) */
   size_t checkpoints_conflict;          /* SAME seq, DIFFERENT checkpoint: a fork */
   vault_witness_continuity_t continuity;/* over the checkpoint run */
   size_t proofs_ok, proofs_unmatched, proofs_bad; /* matched-checkpoint / no-cp / bad */
   size_t snapshots;                     /* leaf-snapshot frames seen */
   size_t snapshots_ok;                  /* digest matches its checkpoint's leaf_snapshot_digest */
   size_t snapshots_unmatched;           /* no checkpoint with that seq in this stream */
   size_t snapshots_bad;                 /* digest disagrees with the signed checkpoint: tamper */
   size_t snapshots_root_mismatch;       /* digest matches but the leaves do not rebuild the root */
   int malformed;                        /* a frame or payload failed to parse */
   int any_tamper;                       /* 1 if any hard tamper was detected */
} vault_witness_offline_report_t;

/* Verify a captured stream. Returns 0 if the stream parsed and the report was
 * produced (report->any_tamper says whether tampering was found), -1 only on a
 * usage error (NULL args). A malformed frame sets report->malformed and any_tamper. */
int vault_witness_offline_verify(const uint8_t *stream, size_t stream_len,
                                 const vault_witness_anchor_t *anchors, size_t n_anchors,
                                 vault_witness_offline_report_t *report);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_VAULT_WITNESS_OFFLINE_H */
