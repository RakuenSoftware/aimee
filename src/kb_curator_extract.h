#ifndef DEC_KB_CURATOR_EXTRACT_H
#define DEC_KB_CURATOR_EXTRACT_H 1

#include <stddef.h> /* size_t */
#include <stdint.h> /* int64_t */

typedef struct
{
   char extract_command[512];
   int max_tokens;
   int max_attempts;
} kb_curator_extract_opts_t;

/* Claim and process one pending extract_doc job.
 * Invokes the canonical provider, writes artifacts to KB_STORE, marks job done or failed.
 * Returns 1 if a job was processed, 0 if queue is empty, -1 on hard error. */
int kb_curator_extract_one(const kb_curator_extract_opts_t *opts);

/* Requeue a job the provider refused, WITHOUT spending an attempt: returns the
 * row to 'pending', gives back the increment ce_claim_job applied, and sets a
 * backoff. Exposed for tests. */
void kb_curator_mark_retry_provider_unavailable(int64_t job_id, int attempts,
                                                const char *error_msg);

/* Code-unit analogue of the extract_doc requeue above. Kept public for the
 * same reason: unit tests pin the durable queue transition directly. */
void kb_curator_mark_retry_provider_unavailable_code(int64_t job_id, int attempts,
                                                     const char *error_msg);

/* Claim and process one pending extract_code_unit job from kb_code_unit_jobs.
 * Reads source evidence and invokes canonical EXTRACT_CODE model execution,
 * then writes grounded code_unit artifacts to KB_STORE.
 * Returns 1 if a job was processed, 0 if queue is empty, -1 on hard error. */
int kb_curator_extract_code_unit_one(const kb_curator_extract_opts_t *opts);

#endif /* DEC_KB_CURATOR_EXTRACT_H */
