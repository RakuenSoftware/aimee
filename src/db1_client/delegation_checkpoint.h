/* db1/delegation_checkpoint.h: durable checkpoints for resumable delegations.
 *
 * When a delegation fails mid-flight and needs to retry from the last
 * known-good step, the checkpoint stores the steps completed so far,
 * the last output, and the failure reason. Keyed by delegation_id.
 *
 * DB1 owns the schema (delegation_id, job_id, steps_completed,
 * last_output, error, attempt, failed_at, created_at).
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_DELEGATION_CHECKPOINT_H
#define DEC_DB1_DELEGATION_CHECKPOINT_H 1

/* What a checkpoint may hand back. These cross the module boundary, so the
   width is the contract's rather than each caller's: the stage cannot see the
   buffer it is filling and would otherwise have to guess. */
#define DB1_CHECKPOINT_STEPS_LEN  2048
#define DB1_CHECKPOINT_ERROR_LEN  512
#define DB1_CHECKPOINT_OUTPUT_LEN 2048

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_DELEGATION_CHECKPOINT_H */
