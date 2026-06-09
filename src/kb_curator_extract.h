#ifndef DEC_KB_CURATOR_EXTRACT_H
#define DEC_KB_CURATOR_EXTRACT_H 1

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

/* Claim and process one pending extract_code_unit job from kb_code_unit_jobs.
 * Reads source file from the filesystem, invokes the sidecar with
 * role="extract_code_unit", writes code_unit artifacts to DB2.
 * Returns 1 if a job was processed, 0 if queue is empty, -1 on hard error. */
int kb_curator_extract_code_unit_one(const kb_curator_extract_opts_t *opts);

#endif /* DEC_KB_CURATOR_EXTRACT_H */
