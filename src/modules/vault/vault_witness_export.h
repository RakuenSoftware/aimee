#ifndef AIMEE_VAULT_WITNESS_EXPORT_H
#define AIMEE_VAULT_WITNESS_EXPORT_H

#include <stddef.h>
#include <stdint.h>

/* P7-witness-e1: deterministic export framing for the log/OTLP path.
 *
 * All evidence bytes — records, signed checkpoints, inclusion proofs — ride the
 * log path (metrics carry numbers only). This module wraps an already-canonical
 * payload in a version-tagged frame so a consumer comparing copies produced by
 * different aimee versions can tell "encoding changed" (export_version_mismatch)
 * from "evidence disagrees" (a byte difference within a matching version).
 *
 * Rendering is deterministic: it consults no clock, hostname, instance id, or
 * other mutable state — only the payload bytes and the fixed version tag. This is
 * the framing layer only; it is not a network client and holds no state. Pure and
 * production-uninvoked in E1.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define VAULT_WITNESS_EXPORT_LABEL "aimee-vault-witness-export-v1"
#define VAULT_WITNESS_EXPORT_VERSION 1
#define VAULT_WITNESS_EXPORT_HEADER_LEN 16

typedef enum
{
   VAULT_WITNESS_EXPORT_RECORD = 0,
   VAULT_WITNESS_EXPORT_CHECKPOINT = 1,
   VAULT_WITNESS_EXPORT_PROOF = 2,
   /* A checkpoint's leaf snapshot: u64 checkpoint_seq (big-endian) followed by the
    * exact stored snapshot bytes the checkpoint's leaf_snapshot_digest covers. With
    * the snapshot in hand a consumer can rebuild the whole tree offline and check
    * every shard head, which is what the cross-gap leaf comparison procedure needs.
    * An older consumer that predates this kind reports it as an unknown frame,
    * which is tolerated and never counted as tampering. */
   VAULT_WITNESS_EXPORT_SNAPSHOT = 3
} vault_witness_export_kind_t;

/* Parse results, kept distinct because they mean different things to an operator:
 * a version mismatch is a rollout artifact, a malformed frame is corruption. */
typedef enum
{
   VAULT_WITNESS_EXPORT_PARSE_OK = 0,
   VAULT_WITNESS_EXPORT_PARSE_MALFORMED,
   VAULT_WITNESS_EXPORT_PARSE_VERSION_MISMATCH
} vault_witness_export_parse_t;

/* Frame a canonical payload. Writes VAULT_WITNESS_EXPORT_HEADER_LEN + payload_len
 * bytes and sets *out_len. Returns 0 on success, -1 on invalid kind, NULL args,
 * or insufficient buffer. */
int vault_witness_export_frame(vault_witness_export_kind_t kind, const uint8_t *payload,
                               size_t payload_len, uint8_t *out, size_t cap, size_t *out_len);

/* Parse a frame. On OK, sets kind and points payload/payload_len into `frame`
 * (no copy). VERSION_MISMATCH is returned for a well-formed header whose export
 * version is not VAULT_WITNESS_EXPORT_VERSION — distinct from MALFORMED so a
 * rollout does not read as tampering. */
vault_witness_export_parse_t vault_witness_export_parse(const uint8_t *frame, size_t frame_len,
                                                        vault_witness_export_kind_t *kind,
                                                        const uint8_t **payload,
                                                        size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_VAULT_WITNESS_EXPORT_H */
