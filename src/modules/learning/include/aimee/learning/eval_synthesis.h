/* learning_eval_synthesis.h: turning a confirmed live failure into a
 * regression eval task — the pure half.
 *
 * Everything here is a pure function over strings: fingerprinting, task
 * rendering, text admissibility, and the admission predicate. No DB access and
 * no filesystem access, so the policy unit-tests standalone and the storage
 * half (eval_synthesis.c, which owns the DB1 candidate ledger and writes the
 * materialised suite file) can be tested against it.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */
#ifndef DEC_LEARNING_EVAL_SYNTHESIS_H
#define DEC_LEARNING_EVAL_SYNTHESIS_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 32 hex chars + NUL, matching DB1_EVAL_CAND_SIGNATURE_LEN. */
#define LEARNING_EVAL_SIGNATURE_LEN 33
#define LEARNING_EVAL_TASK_NAME_LEN 128

/* Default reproduction bar: a failure must be seen this many times, from
 * distinct sessions, before it is admitted to a suite. */
#define LEARNING_EVAL_MIN_OCCURRENCES 2

/* Bound on any single synthesised text field. Deliberately small: a
 * regression task is a focused check, not a transcript. */
#define LEARNING_EVAL_MAX_FIELD 512

   /* A confirmed failure, normalised across its sources. All fields are
    * borrowed; none may contain untrusted markup (see
    * learning_eval_text_admissible). */
   typedef struct
   {
      const char *origin;       /* eval_failure|agent_outcome|correction|dogfood|verify */
      const char *origin_ref;   /* pointer back to the row/artifact that raised it */
      const char *role;         /* agent role; NULL/empty renders as "execute" */
      const char *prompt;       /* the task prompt to replay */
      const char *failure_mode; /* short description of what went wrong */
      /* The success check, in the vocabulary the harness already evaluates
       * (modules/benchmarks/agent_eval.c): "contains" tests the response for a
       * substring; "exit_code" and an EMPTY type both fall through to "the
       * agent succeeded".
       *
       * An empty check is not a gap — it is the classic regression shape: this
       * prompt used to fail, it must now succeed. That matters because most
       * failure records carry a replayable prompt but no statement of what
       * success would have looked like, and inventing one would be fabrication. */
      const char *check_type;
      const char *check_value; /* required when check_type is "contains" */
   } learning_eval_failure_t;

   /* Is this string safe to persist into an agent-visible task?
    *
    * This is a conservative ALLOWLIST, not a sanitizer: it rejects control
    * characters and the markup characters that carry prompt-injection
    * structure (< > [ ] | # \ ` $ { }) rather than rewriting them. It is
    * deliberately stricter than the render-boundary prompt sanitizer in
    * src/kb/prompt_sanitizer.c, and is NOT a substitute for it — that boundary
    * owns rendering untrusted text into a prompt; this one refuses to store it
    * at all. Synthesis fails closed on a field this rejects, so no laundering
    * path exists through the ledger.
    *
    * Returns 1 when admissible, 0 otherwise. NULL is admissible (absent), an
    * over-long field is not. */
   int learning_eval_text_admissible(const char *s);

   /* Every field of `f` that becomes stored text is admissible. */
   int learning_eval_failure_admissible(const learning_eval_failure_t *f);

   /* Stable fingerprint over (role, failure_mode, check_type, check_value),
    * normalised so cosmetic differences collapse: case-folded, every run of
    * non-alphanumeric characters becomes one space, ends trimmed. Writes
    * LEARNING_EVAL_SIGNATURE_LEN bytes. Returns 0 on success, -1 on bad args,
    * -2 when a field is inadmissible. */
   int learning_eval_signature(const learning_eval_failure_t *f, char *out, size_t out_len);

   /* Deterministic, filesystem-safe task name derived from the signature.
    * Returns 0 on success, -1 on bad args. */
   int learning_eval_task_name(const char *signature, char *out, size_t out_len);

   /* Render the task JSON in the format agent_eval_load_tasks() parses, plus a
    * `provenance` object recording that this task was synthesised and from
    * what. A "contains" check without a value is rejected; an empty check type
    * renders no success_check at all, leaving the harness's own success
    * verdict as the bar. Requires a non-empty prompt — a task with nothing to
    * replay is not a task. Returns 0 on success, -1 on bad args / overflow,
    * -2 when a field is inadmissible. */
   int learning_eval_build_task(const learning_eval_failure_t *f, const char *task_name, char *out,
                                size_t out_len);

   /* Admission predicate. A candidate is admitted only when it has reproduced
    * enough times FROM DISTINCT SESSIONS and the endogeneity gate is open —
    * one session repeating itself is not reproduction, and a loop feeding on
    * its own output does not get to widen its own yardstick.
    * Returns 1 to admit, 0 to hold. */
   int learning_eval_admission_ready(int occurrences, int distinct_sessions, int min_occurrences,
                                     int gate_open);

#ifdef __cplusplus
}
#endif

#endif /* DEC_LEARNING_EVAL_SYNTHESIS_H */
