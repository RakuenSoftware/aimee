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
 * Invokes the sidecar, writes artifacts to DB2, marks job done or failed.
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
 * Reads source file from the filesystem, invokes the sidecar with
 * role="extract_code_unit", writes code_unit artifacts to DB2.
 * Returns 1 if a job was processed, 0 if queue is empty, -1 on hard error. */
int kb_curator_extract_code_unit_one(const kb_curator_extract_opts_t *opts);

/* Resolve the curator-extract.py sidecar command shared by the doc and code
 * extract stages. An explicit opts->extract_command wins; otherwise the bundled
 * script is located at its real install path and run as `python3 <path>`. */
void kb_curator_resolve_sidecar_command(const kb_curator_extract_opts_t *opts, char *out,
                                        size_t len);

/* Core selection, exposed for testing: an explicit command wins; else the first
 * READABLE candidate is run as `python3 <path>` (the script is invoked via
 * python3, so readability — not the execute bit — is what matters); else a
 * cwd-relative last resort. NULL/empty candidate entries are skipped. */
void kb_curator_pick_sidecar_command(const char *explicit_cmd, const char *const *candidates,
                                     int n_candidates, char *out, size_t len);

#endif /* DEC_KB_CURATOR_EXTRACT_H */
