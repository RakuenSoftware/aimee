#ifndef DEC_WORKSPACE_H
#define DEC_WORKSPACE_H 1

#include "aimee.h"

#define MAX_DISCOVERED_PROJECTS 256
#define MAX_WORKSPACE_DEPTH     10

/* Recursively discover git repos under root, up to max_depth levels.
 * When a .git directory is found, the path is added and recursion stops
 * for that subtree. Returns count of discovered projects, -1 on error. */
int workspace_discover_projects(const char *root, int max_depth, char projects[][MAX_PATH_LEN],
                                int max);

/* Build session context from config workspaces + DB projects.
 * Reads describe files and style files for indexed projects.
 * Caller owns returned string. */
char *workspace_build_context_from_config(const config_t *cfg);

/* Resolve the active workspace root for a command running from cwd.
 * Prefers the current git/worktree top-level, then the most specific configured
 * workspace containing cwd, then cwd itself. Returns 0 on success. */
int workspace_active_root(const config_t *cfg, const char *cwd, char *out, size_t out_len);

/* Resolve a path-independent repository identity for memory scoping.
 *
 * For a git repo this yields the canonical remote identity rather than the
 * local checkout path, so two clones of the same repo on different machines
 * resolve to the same scope (the cross-environment recall bug otherwise):
 *   project_out   = canonical repo URL  (https://host/owner/repo) or
 *                   local:<repo-root> for a repo with no usable remote.
 *   workspace_out = the repo's org/namespace parent (https://host/owner) when
 *                   it has one, else the repo identity itself.
 * Either out pointer may be NULL. Returns 0 when a repo identity was resolved,
 * -1 otherwise (cwd is not inside a resolvable git repo) — callers then keep
 * their legacy path/basename labels. */
int workspace_repo_identity(const char *cwd, char *project_out, size_t project_len,
                            char *workspace_out, size_t workspace_len);

/* Canonical index keys for a discovered repo root: the project name and its
 * workspace, used to key the shared db2 index (projects.name, kb_documents.project)
 * and the ingest queue. Prefers the repository identity (workspace_repo_identity)
 * so a repo indexed from any machine resolves to one project; falls back to the
 * directory basename and `fallback_workspace` for non-repo roots. Always writes
 * both outputs (never fails). */
void workspace_repo_index_keys(const char *root, const char *fallback_workspace, char *name_out,
                               size_t name_len, char *ws_out, size_t ws_len);

/* Resolve a proposal path: try path as is, then search docs/proposals/ subdirectories.
 * Returns newly allocated absolute path string, or NULL if not found. Caller frees. */
char *resolve_proposal_path(const char *proposal);

/* Read a project's style guide from ~/.config/aimee/projects/<name>.style.md.
 * Returns malloc'd string or NULL. Caller frees. */
char *style_read(const char *project_name);

int worktree_sibling_path(const char *git_root, const char *sid, const char *work_name, char *out,
                          size_t out_len);
/* Deterministic per-delegation worktree work-name derived from `sid` (FNV-1a
 * over the session key — its leading characters; see worktree_session_key in
 * workspace.c), so the dispatch and server-compute paths resolve to one shared
 * sibling worktree instead of two. Writes <=8 hex chars + NUL into out[cap]
 * (cap >= 9). Returns 0 on success, -1 on bad args. */
int worktree_delegate_work_name(const char *sid, char *out, size_t cap);
int is_aimee_worktree_path(const char *path);
int worktree_managed_git_root(const char *path, char *out, size_t out_len);
int worktree_create_sibling(const char *git_root, const char *sid, const char *work_name);
int worktree_create_sibling_from_ref(const char *git_root, const char *sid, const char *work_name,
                                     const char *base_ref);
int worktree_create_sibling_from_anchor(const char *git_root, const char *sid,
                                        const char *work_name, const char *anchor_dir);
/* Replay the anchor worktree's uncommitted changes (tracked diff + untracked,
 * non-ignored files) into the freshly created worktree at wt_path. Both must sit
 * at the same base commit. Best-effort; logs and continues on partial failure.
 * Called by worktree_create_sibling_from_anchor; exposed for unit tests. */
void worktree_apply_anchor_wip(const char *anchor_dir, const char *wt_path);
int worktree_create_sibling_on_branch(const char *git_root, const char *sid, const char *work_name,
                                      const char *branch, const char *anchor_dir);
void worktree_cleanup(const char *git_root, const char *sid, const char *work_name);
int worktree_apply_changes_to_parent(const char *src_wt, const char *dst_wt);
int worktree_apply_delegate_changes_to_parent(const char *delegate_wt, const char *parent_wt,
                                              char *err, size_t err_len);
int worktree_apply_delegate_changes_checked(const char *delegate_wt, const char *parent_hint,
                                            const char *launch_head, int *applied,
                                            char *parent_root, size_t parent_root_len, char *err,
                                            size_t err_len);
void worktree_registry_record(const char *git_root, const char *wt_path, const char *branch,
                              const char *sid, const char *work_name);
int worktree_find_branch_in_repo(const char *git_root, const char *branch, char *out_dir,
                                 size_t out_len);
int worktree_find_branch_registered(const char *branch, char *out_dir, size_t out_len);
int check_merged_pr_for_branch(const char *git_dir);
const char *worktree_for_cwd(const session_state_t *state, const char *cwd);

/* Detect the base branch for a new worktree rooted at git_root.
 * Prefers the repository's DEFAULT branch (origin/HEAD), then local
 * main/master/trunk, then the current HEAD as a last resort. Writes result
 * into buf (at most buf_len bytes including NUL). */
void worktree_detect_base_branch(const char *git_root, char *buf, size_t buf_len);

/* Count active aimee-managed worktrees for git_root. */
int count_active_worktrees_for_root(const char *git_root);

/* Decide whether the session needs to relocate into its per-session
 * worktree for isolation. Returns 1 and fills `target` with the worktree
 * path when cwd is inside a main (non-aimee) git checkout; returns 0 when
 * cwd is already inside an aimee-managed worktree, is not in a git repo, or
 * the path/creation lookup fails. When create_if_missing is non-zero the
 * worktree is created on demand (idempotent; reuses an existing one). */
int session_isolation_target(const char *cwd, const char *sid, char *target, size_t target_len,
                             int create_if_missing);

#endif /* DEC_WORKSPACE_H */
