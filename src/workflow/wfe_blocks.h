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
} wfe_forge_t;

/* Install a forge provider (NULL restores the default live/gh provider). */
void wfe_set_forge_provider(const wfe_forge_t *p);

#endif /* DEC_WFE_BLOCKS_H */
