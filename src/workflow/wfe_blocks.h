/* wfe_blocks.h: the non-gate block executors (author/implement/freeze/pr/merge)
 * and a few standalone, unit-testable helpers (git freeze). Registered behind
 * the wfe_iface vtable via wfe_register_default_executors(). */
#ifndef DEC_WFE_BLOCKS_H
#define DEC_WFE_BLOCKS_H 1

#include <stddef.h>

/* Register the real executors for every non-gate block type. Gates are
 * registered by their own slices (W4 human, W5 roundtable). */
void wfe_register_default_executors(void);

/* Compute a frozen diff for the implementation gate. base_ref defaults to the
 * merge-base of HEAD against `base_branch` (e.g. "origin/testing" or "main").
 * Fills the short SHAs and the sha256 hex of the cumulative diff. Returns 0 on
 * success. Standalone (no engine ctx) so it is directly unit-testable. */
int wfe_git_freeze(const char *repo_dir, const char *base_branch, char out_base_sha[64],
                   char out_head_sha[64], char out_diff_hash[65], char *err, size_t errlen);

/* ---- Forge seam for the safety blocks (gate.ci / check.mergeable / merge).
 * The default provider calls `gh` and is integration-gated; tests inject a mock
 * so the state-mapping + idempotent-merge logic is unit-testable. ---- */
typedef enum
{
   WFE_CI_SUCCESS = 0, /* all checks green */
   WFE_CI_FAILURE,     /* any failed/error/cancelled/timed_out */
   WFE_CI_PENDING,     /* still running */
   WFE_CI_NONE         /* no checks / unknown / unreachable -> fail closed */
} wfe_ci_status_t;

typedef enum
{
   WFE_MERGE_OK = 0,        /* merged now */
   WFE_MERGE_ALREADY,       /* already merged -> idempotent no-op success */
   WFE_MERGE_NOT_MERGEABLE, /* conflict / lost race -> loop */
   WFE_MERGE_ERROR          /* forge error -> fail closed */
} wfe_merge_result_t;

typedef struct
{
   wfe_ci_status_t (*ci_status)(const char *repo, const char *pr_ref);
   int (*mergeable)(const char *repo, const char *pr_ref); /* 1 yes, 0 conflict, -1 unknown */
   int (*is_merged)(const char *repo, const char *pr_ref); /* 1 merged, 0 open, -1 unknown */
   wfe_merge_result_t (*merge)(const char *repo, const char *pr_ref);
   /* Push `branch` and open a PR; write its forge ref (number/url) into out_pr_ref.
    * Returns 0 on success, -1 on failure. May be NULL on a provider that predates
    * this field (the default live provider and test mocks) -> pr.open fails closed
    * and the gate that follows re-loops. */
   int (*open)(const char *repo, const char *branch, const char *title, const char *body,
               char out_pr_ref[128]);
} wfe_forge_t;

/* Install a forge provider (NULL restores the default live/gh provider). */
void wfe_set_forge_provider(const wfe_forge_t *p);

/* ---- Delegate seam for the producing blocks (author/implement/document).
 * The wfe library must not depend on server/ delegate internals
 * (module-boundary-check), so blocks dispatch real delegate work through this
 * registered hook. The default provider is NULL -> producing blocks fail closed
 * (the gate that follows re-loops), exactly the pre-seam integration-gated
 * behavior. The server registers a live provider that runs delegates (decompose
 * -> fan out -> verify -> re-delegate is owned by the live provider; see the
 * full-autonomous-development plan). Tests inject a mock. ---- */
typedef struct
{
   /* Run the block's delegate work in `workdir` as `role` with `prompt`.
    * `delegate` is the step's assigned agent name, the sentinel "$random" (resolve
    * to a random roster agent), or "" to route by `role`. If the block produces a
    * file artifact, `artifact_path` is its path (else NULL). On success returns 0
    * and, if a commit was made, fills out_commit_sha (else ""). Non-zero => the
    * caller emits failed/looped, never a crash. */
   int (*run)(const char *workdir, const char *role, const char *delegate, const char *prompt,
              const char *artifact_path, char out_commit_sha[64], char *err, size_t errlen);
} wfe_delegate_provider_t;

/* Install a delegate provider (NULL restores the default fail-closed provider). */
void wfe_set_delegate_provider(const wfe_delegate_provider_t *p);

/* ---- Mechanical verify seam for the implement manager loop (WP-1b).
 * implement only advances a unit that PASSES the mechanical gate. The wfe library
 * must not call the server's git_verify directly (module-boundary-check), so it
 * runs verification through this registered hook, which returns the structured
 * (git_verify format=json) verdict. The server registers a live provider that
 * calls handle_git_verify; tests inject a mock. Default NULL -> implement skips
 * verification (the pre-WP-1b behavior), so the engine stays drivable without a
 * provider. ---- */
typedef struct
{
   /* Run the mechanical verify gate on `workdir`, writing the structured verdict
    * JSON (the git_verify format=json document) into out_verdict. Returns 0 if a
    * verdict was produced, -1 if the gate could not run at all (treated as a
    * non-pass — fail closed). */
   int (*verify)(const char *workdir, char *out_verdict, size_t n);
} wfe_verify_provider_t;

/* Install a verify provider (NULL = implement does not gate on verification). */
void wfe_set_verify_provider(const wfe_verify_provider_t *p);

/* The implement verify gate: 1 = advance (verdict passed, or no provider -> skip),
 * 0 = block (anything that is not an explicit pass -> fail closed). Exposed for the
 * unit test. */
int wfe_implement_verify_ok(const char *workdir);

#endif /* DEC_WFE_BLOCKS_H */
