#ifndef DEC_GIT_VERIFY_H
#define DEC_GIT_VERIFY_H 1

#include "cJSON.h"
#include "server.h"

#define MAX_VERIFY_STEPS 16
#define MAX_STEP_NAME    64
#define MAX_STEP_CMD     512
#define MAX_ENV_CHECKS   16
#define VERIFY_TTL_SECS  3600 /* kept for display only; no longer enforced as a gate */

typedef enum
{
   VERIFY_ACTION_RUN,
   VERIFY_ACTION_CHECK,
   VERIFY_ACTION_CONFLICTS,
   VERIFY_ACTION_ENV,
   VERIFY_ACTION_PREPARE_PR,
   VERIFY_ACTION_STATUS,
   VERIFY_ACTION_INSTALL_HOOK
} verify_action_t;

typedef struct
{
   char name[MAX_STEP_NAME];
   char run[MAX_STEP_CMD];
   char after[MAX_STEP_NAME]; /* empty = no dependency; else wait for named step */
   char paths[256];           /* comma-separated changed-path globs for incremental verify */
   int scope_changed;         /* 1 = expose changed-file env to this step */
} verify_step_t;

typedef struct
{
   verify_step_t steps[MAX_VERIFY_STEPS];
   int count;
   char env_checks[MAX_ENV_CHECKS][MAX_STEP_NAME];
   int env_count;
   int enforce; /* 1 = gate push/PR create; 0 = informational only (default) */
   int incremental;
   char always_run_globs[256];
} verify_config_t;

/* Load verify steps from .aimee/project.yaml. Returns 0 on success, -1 if no
 * verify section found. project_root may be NULL (uses cwd). */
int verify_load_config(const char *project_root, verify_config_t *cfg);

/* Verify scope gate. Returns 1 if target_repo_root is in scope for
 * verification — either cross-project verify is enabled in config, or the
 * target resolves to the same canonical main repo as one of the session's
 * registered worktrees (i.e. it is the session's current project). Returns 0
 * for a cross-project repository when cross-project verify is disabled (the
 * default). A session with no registered worktree mapping is treated as in
 * scope so plain single-repo CLI use is unaffected. project_root may be NULL.
 *
 * When this returns 0, callers must skip verify entirely: do not load or
 * auto-generate project.yaml, do not run steps, and do not gate the push/PR. */
int verify_project_in_scope(const char *target_repo_root);

/* Check whether the last verification is still valid.
 * Valid means: tree hash of HEAD matches a stored entry AND within TTL (3600s).
 * expected_commit: if non-NULL, this commit SHA is resolved to its tree hash
 *   and compared against stored tree hashes (used by the pre-push hook so that
 *   squash-merges and rebases with the same content still pass the gate).
 *   Pass NULL to fall back to the tree hash of the current HEAD.
 * Writes explanatory message to msg_buf. If no verify section in project.yaml,
 * returns 1 (no gate). Returns 1 if valid, 0 if verification required. */
int verify_check(const char *project_root, const char *expected_commit, char *msg_buf,
                 size_t msg_len);

/* Unified push/PR verify gate. Returns 1 if the operation should be BLOCKED
 * (msg filled with the reason), 0 if allowed. Encapsulates scope
 * (verify_project_in_scope), the global verify_enabled master switch, config
 * resolution (no auto-generation for an unconfigured repo while verify is
 * disabled), the per-project enforce flag, and the freshness check. msg may be
 * NULL. target_root may be NULL; expected_commit may be NULL. */
int verify_gate_blocks(const char *target_root, const char *expected_commit, char *msg,
                       size_t msg_len);

/* Compute the tree hash (HEAD^{tree}) for the given directory.
 * Using the tree hash rather than the commit hash means squash-merges and
 * rebases that don't change content are still recognised as verified.
 * Returns a hex string (caller must free), or NULL on failure. */
char *verify_compute_file_hash(const char *project_root);

/* MCP tool handler: git_verify.
 *
 * Pass the server context to allow async verify from the server. Async verify
 * allocates a dedicated secondary pool so long runs cannot starve ctx->pool.
 * Callers without a server context (e.g. cmd_infra's CLI in-process path
 * for builds running without a live server) may pass NULL; verify_run_waves
 * then falls back to an ephemeral pool. */
cJSON *handle_git_verify(server_ctx_t *server_ctx, cJSON *args, const char *session_id);

/* Mark active background verification jobs owned by session_id for cancellation.
 * Running child processes observe the flag and are terminated promptly. Returns
 * the number of jobs marked. */
int verify_cancel_session(const char *session_id);

/* Build the absolute path of the global project.yaml for the given
 * project. project_root may be NULL (uses cwd). Resolves through
 * worktrees so worktrees and the main checkout share one file.
 * Returns 0 on success, -1 if HOME or project name cannot be resolved. */
int project_yaml_path(const char *project_root, char *out, size_t out_len);

/* Read primary_branch from the project's project.yaml.
 * Returns 0 and writes branch name into out if set, -1 if absent or unreadable. */
int project_primary_branch(const char *project_root, char *out, size_t out_len);

/* Install a pre-push git hook that checks the aimee verify gate before each
 * push. The hook calls `aimee git verify action=check` and blocks the push
 * when the last verification run has failed steps or is stale.
 *
 * Returns 0 on success, -1 on I/O failure, -2 if a non-aimee hook already
 * exists (left unchanged to avoid clobbering the user's existing hook).
 *
 * project_root may be NULL (uses cwd). The hook is written to the git
 * common-dir (so worktrees and the main checkout share one hook). */
int verify_install_git_hook(const char *project_root);

/* Branch ownership operations have moved to branch_ownership.h. Callers
 * that need them should include "branch_ownership.h" directly. */

#endif /* DEC_GIT_VERIFY_H */
