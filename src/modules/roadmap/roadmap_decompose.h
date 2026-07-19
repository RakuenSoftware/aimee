/* roadmap_decompose.h: goal-to-roadmap JSON decomposition module.
 *
 * A pure-C module (no agent/DB/network deps) that turns a goal into a
 * validated roadmap decomposition JSON by calling a caller-supplied model
 * function and gating every result through the existing structural validator
 * (roadmap_validate_decomposition).
 *
 * The decompose orchestration is unit-testable without a live model because
 * the model hook is injectable.
 */

#ifndef DEC_ROADMAP_DECOMPOSE_H
#define DEC_ROADMAP_DECOMPOSE_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Model hook: given a fully-formed prompt, produce the model's raw text
    * reply heap-allocated in *out_text (caller frees). Return 0 on success,
    * -1 on error. Kept injectable so the decompose orchestration is
    * unit-testable without a live model. */
   typedef int (*roadmap_model_fn)(const char *prompt, char **out_text, void *ud);

   /* Build the decomposition prompt for a goal. prior_err (may be NULL/empty)
    * is the validation failure from the previous attempt, fed back so the
    * model can correct. Returns a heap string in *out (caller frees), 0 on
    * success / -1 on error. */
   int roadmap_decompose_build_prompt(const char *goal, const char *planning_depth,
                                      const char *token_profile, const char *prior_err, char **out);

   /* Extract the first balanced top-level JSON object ({...}) from arbitrary
    * model text, tolerating prose and ```json fences before/after. Brace
    * scanning MUST respect string literals and backslash escapes (a { or }
    * inside a JSON string does not change depth). Returns a heap copy of the
    * object text in *out (caller frees), 0 on success; -1 if no balanced object
    * is found. */
   int roadmap_decompose_extract_json(const char *text, char **out);

   /* Run the decompose loop: up to `attempts` times, build a prompt (feeding
    * back the prior validation error), call model(), extract the JSON object,
    * and validate it with roadmap_validate_decomposition. The first attempt
    * whose extracted JSON validates wins: its JSON is returned heap-allocated
    * in *out_json (caller frees) and the function returns 0. If no attempt
    * yields valid JSON, writes a human-readable reason into err (if non-NULL)
    * and returns -1. attempts <= 0 is treated as 1. No LLM output is ever
    * returned without passing structural validation. */
   int roadmap_decompose_run(const char *goal, const char *planning_depth,
                             const char *token_profile, roadmap_model_fn model, void *ud,
                             int attempts, char **out_json, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ROADMAP_DECOMPOSE_H */