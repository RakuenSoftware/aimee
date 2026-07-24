#ifndef AIMEE_VAULT_WITNESS_VERIFY_H
#define AIMEE_VAULT_WITNESS_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "vault_witness_checkpoint.h"
#include "vault_witness_merkle.h"
#include "vault_witness_record.h"

/* P7-witness-e2: offline verification over emitted bytes only.
 *
 * These are the checks an operator's offline tool and the continuous verifier
 * both run. They consult no database — only the emitted records/checkpoints plus
 * an out-of-band trust anchor — so a compromised database cannot influence the
 * verdict. This is the "detection by comparison" primitive: given a copy of the
 * emitted stream, decide whether it is internally consistent and whether it links
 * to what a signed checkpoint committed to.
 */

#ifdef __cplusplus
extern "C"
{
#endif

/* Result of verifying a per-shard run of witness records. On a break, `break_index`
 * is the 0-based index of the first record that fails, for operator triage. */
typedef enum
{
   VAULT_WITNESS_CHAIN_OK = 0,
   VAULT_WITNESS_CHAIN_EMPTY,          /* zero records — nothing to verify */
   VAULT_WITNESS_CHAIN_BAD_RECORD,     /* a record fails structural validity/digest */
   VAULT_WITNESS_CHAIN_BAD_GENESIS,    /* first record is not a valid first-in-shard */
   VAULT_WITNESS_CHAIN_SEQ_GAP,        /* shard_seq not contiguous +1 */
   VAULT_WITNESS_CHAIN_SHARD_MISMATCH, /* a record belongs to a different shard */
   VAULT_WITNESS_CHAIN_BROKEN_LINK     /* witness_pred does not match prior digest */
} vault_witness_chain_result_t;

/* Verify that `records[0..n)` form a contiguous, correctly linked run for a single
 * shard: record 0 is first-in-shard with the genesis predecessor, each subsequent
 * record's shard_seq increments by one, all share one (tenant, provider), and each
 * record's witness_pred_hash equals the digest of the record before it. Records
 * must already be decoded. Returns OK or the first failure; on failure and if
 * `break_index` is non-NULL, writes the offending index. This is the check that
 * catches a locally inconsistent tampering of the emitted stream. */
vault_witness_chain_result_t vault_witness_verify_chain(const vault_witness_record_t *records,
                                                        size_t n, size_t *break_index);

/* Verify that a shard's head is included under a signed checkpoint root. The
 * consumer supplies the shard IDENTITY (tenant, provider, sequence, head_hash) and
 * an inclusion proof; this recomputes the SMT key and leaf hash FROM the identity
 * — never trusting a caller-supplied key/leaf — and checks the proof against
 * `root`. That binding is what stops an attacker presenting one shard's leaf at
 * another's position. Returns 1 if included, 0 otherwise (including bad input). */
int vault_witness_verify_inclusion(const char *tenant, const char *provider, uint64_t sequence,
                                   const uint8_t head_hash[32],
                                   const uint8_t proof[VAULT_WITNESS_SMT_DEPTH][32],
                                   const uint8_t root[32]);

/* Continuity over a run of checkpoints the consumer holds, each already verified
 * for signature. Returns the strongest verdict that holds across the run:
 *   - CONTINUITY_OK if every adjacent pair links (predecessor digest matches),
 *   - CONTINUITY_UNPROVEN if the run has a gap (a checkpoint whose predecessor is
 *     not the immediately preceding held checkpoint) — a work item, not a pass,
 *   - CONTINUITY_BROKEN if an adjacent pair actively disagrees.
 * `gap_after_index`, when non-NULL, receives the index after which the first gap
 * or break occurs, so the caller can surface the cross-gap leaf sets. Checkpoints
 * must be supplied in ascending seq order. */
vault_witness_continuity_t vault_witness_verify_checkpoint_run(
    const vault_witness_checkpoint_t *checkpoints, size_t n, size_t *gap_after_index);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_VAULT_WITNESS_VERIFY_H */
