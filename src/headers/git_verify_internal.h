#ifndef GIT_VERIFY_INTERNAL_H
#define GIT_VERIFY_INTERNAL_H

#include "git_verify.h"
#include "aimee.h" /* MAX_PATH_LEN */

/* Per-step execution context. Used by verify_run_waves and verify_prepare_pr
 * (in git_verify_ops.c). Not part of the public API. */
typedef struct
{
   verify_step_t *step;
   int index;
   int total;
   int rc;
   double elapsed;
   char *output;
   char project_root[MAX_PATH_LEN]; /* thread-local CWD; empty = process CWD */
   int skipped;                     /* 1 = cached pass at current tree hash; step was not re-run */
   char skip_reason[128];
   char changed_files_path[MAX_PATH_LEN];
   char changed_matched[1024];
   char baseline_ref[64];
   int changed_all;
   volatile int *cancel_requested;
} verify_thread_ctx_t;

/* Run verify steps with dependency ordering.
 * Back-compat wrapper: allocates an ephemeral pool for CLI-only callers.
 * Server-side callers use verify_run_waves_on_pool directly. */
void verify_run_waves(verify_config_t *cfg, verify_thread_ctx_t *contexts);
void verify_run_step(verify_thread_ctx_t *ctx);
void verify_config_prefer_verify_local(const char *project_root, verify_config_t *cfg);

/* Resolve dir to the canonical main-repo root (worktrees collapse to the shared
 * repo). Defined in git_verify.c; used by the scope gate in git_verify_ops.c. */
int resolve_main_repo_root(const char *dir, char *out, size_t out_len);

/* Read the global verify master switch (config verify_enabled). Defined in
 * git_verify_ops.c; used by generate_project_yaml in git_verify.c. */
int verify_enabled_global(void);

/* git_verify_ops.c — action handlers dispatched by handle_git_verify */
char *verify_resolve_conflicts(const char *project_root);
char *verify_check_env(verify_config_t *cfg);
char *verify_prepare_pr(const char *project_root, const char *base_branch);
void verify_state_path(const char *project_root, char *buf, size_t len);

#endif /* GIT_VERIFY_INTERNAL_H */
