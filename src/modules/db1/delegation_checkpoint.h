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

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Upsert checkpoint for `delegation_id`. 0 on success, -1 on error. */
   int db1_delegation_checkpoint_save(const char *delegation_id, const char *job_id, int attempt,
                                      const char *steps_json, const char *last_output,
                                      const char *error);

   /* Read checkpoint for `delegation_id`. Buffers may be NULL to skip.
    * Returns 0 on hit, -1 on miss or error. */
   int db1_delegation_checkpoint_load(const char *delegation_id, char *steps_out, size_t steps_cap,
                                      char *error_out, size_t error_cap, char *output_out,
                                      size_t output_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_DELEGATION_CHECKPOINT_H */
