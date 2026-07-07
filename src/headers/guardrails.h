#ifndef DEC_GUARDRAILS_H
#define DEC_GUARDRAILS_H 1

#include <pthread.h>
#include <stdatomic.h>

#define MAX_WORKTREES  16
#define MAX_SEEN_PATHS 64
#define MAX_SEEN_LEN   512

/* Read-before-write and stale-edit detection */
#define MAX_READ_PATHS  64
#define MAX_FILE_HASHES 64

/* Per-session anti-pattern hit tracking. Scoped to the session state file so
 * counts naturally reset when a new session starts and can be cleared via
 * `aimee memory antipattern reset`. */
#define MAX_AP_SESSION_HITS    32
#define AP_HIT_BLOCK_THRESHOLD 3

typedef struct
{
   int64_t pattern_id;
   int hits;
} ap_session_hit_t;

/* Per-file read record: path and FNV-1a hash of file contents at read time */
typedef struct
{
   char path[MAX_SEEN_LEN];
   uint64_t content_hash;
} file_read_hash_t;

/* TDD write tracking: compact stems (not full paths) to keep session_state_t small.
 * MAX_TDD_STEM is the max stem length tracked; stems longer than this are truncated. */
#define MAX_TDD_WRITES 8
#define MAX_TDD_STEM   32

typedef struct
{
   char stem[MAX_TDD_STEM]; /* logical stem, e.g. "foo" for test_foo.c / foo.c */
   int is_test;             /* 1 = test file, 0 = implementation file */
} tdd_write_t;

typedef struct
{
   char path[MAX_PATH_LEN];
   severity_t severity;
   char reason[256];
} classification_t;

/* Simple worktree mapping: git repo root -> aimee-managed worktree path */
typedef struct
{
   char git_root[MAX_PATH_LEN];      /* original repo root (e.g. /root/dev/aimee) */
   char worktree_path[MAX_PATH_LEN]; /* repo-local worktree under .aimee/worktrees */
} worktree_mapping_t;

typedef struct
{
   char seen_paths[MAX_SEEN_PATHS][MAX_SEEN_LEN];
   int seen_count;
   char session_mode[16];
   char guardrail_mode[16];
   int64_t active_task_id;
   int hook_call_count; /* increments each pre_tool_check call for diagnostics */
   int dirty;

   /* Active worktree mappings for this session */
   worktree_mapping_t worktrees[MAX_WORKTREES];
   int worktree_count;

   /* Orchestrator self-discipline tracking.
    * is_delegate: set at runtime (not persisted) when running as an aimee delegate
    *              agent so orchestrator checks are suppressed.
    * orch_direct_edits: count of direct source-file edits by the orchestrator.
    * orch_nudge_sent: 1 once the general delegation nudge has been emitted. */
   int is_delegate;
   int orch_direct_edits;
   int orch_nudge_sent;

   /* Skill dispatch advisory tracking. */
   int skill_find_symbols_advisory_sent;
   int skill_condition_waiting_advisory_sent;
   int skill_tdd_advisory_sent;

   /* TDD enforcement.
    * tdd_mode: "off" (default, unenforced), "warn" (advisory), or "enforce" (block).
    * tdd_writes: compact stem-based write record for test/impl ordering.
    * tdd_write_count: number of entries. */
   char tdd_mode[16];
   tdd_write_t tdd_writes[MAX_TDD_WRITES];
   int tdd_write_count;

   /* Read-before-write tracking: absolute paths read via the Read tool this session. */
   char read_paths[MAX_READ_PATHS][MAX_SEEN_LEN];
   int read_path_count;

   /* Stale-edit detection: FNV-1a hash of file contents at last Read time. */
   file_read_hash_t file_hashes[MAX_FILE_HASHES];
   int file_hash_count;

   /* Per-session anti-pattern hit tracking (see MAX_AP_SESSION_HITS). */
   ap_session_hit_t ap_hits[MAX_AP_SESSION_HITS];
   int ap_hit_count;
} session_state_t;

/* Record that a file was read in this session (updates read-set and content hash). */
void session_record_read(session_state_t *state, const char *abs_path);

/* Return 1 if abs_path is in the session read set. */
int session_path_was_read(const session_state_t *state, const char *abs_path);

/* Return the stored content hash for abs_path, or 0 if not recorded. */
uint64_t session_file_hash(const session_state_t *state, const char *abs_path);

/* Compute FNV-1a hash of file at path. Returns 0 on error. */
uint64_t file_content_hash(const char *path);

/* Return 1 if the file at path still contains needle as a substring. */
int file_contains_substring(const char *path, const char *needle);

/* Scan C/H source content for bare (unescaped) newlines inside string literals.
 * Returns 1 if found, 0 if clean. Used by the Write guardrail to block delegates
 * that accidentally write a real newline instead of the \n escape sequence. */
int c_source_has_bare_string_newline(const char *content);

/* Check if a filename matches sensitive patterns (substring match). */
int is_sensitive_file(const char *path);

/* Classify a file path by sensitivity and blast radius. */
classification_t classify_path(const char *file_path);

/* Pre-tool check. Returns exit code:
 *   0 = allow (msg_buf may contain warnings)
 *   1 = allow with path rewrite (msg_buf contains rewritten absolute file path)
 *   2 = block (msg_buf contains error message) */
int pre_tool_check(const char *tool_name, const char *input_json, session_state_t *state,
                   const char *guardrail_mode, const char *cwd, char *msg_buf, size_t msg_len);

/* Register a config-reload reapplier that clears the cached audit_action_enabled
 * / audit_worm_enabled gates so a live config.set / SIGHUP takes effect without a
 * restart. Call once at server startup. */
void guardrails_action_audit_register_reload(void);

/* The guardrail verdict logic. pre_tool_check is a thin wrapper (in
 * guardrails_action_audit.c) that calls this and emits the per-action audit
 * row. Same return contract as pre_tool_check. */
int pre_tool_check_inner(const char *tool_name, const char *input_json, session_state_t *state,
                         const char *guardrail_mode, const char *cwd, char *msg_buf,
                         size_t msg_len);

/* Normalize provider/internal tool names into guardrail-facing categories.
 * Provider-native sub-agent spawns (Task/Agent/spawn_agent/RemoteTrigger) map to
 * "Subagent" — callers test for that to detect a sub-agent call. */
const char *guardrails_canonical_tool_name(const char *tool_name);

/* Post-tool update: re-index edited files and track TDD writes.
 * state may be NULL (TDD tracking is skipped). */
void post_tool_update(const char *tool_name, const char *input_json, session_state_t *state);

/* Return 1 if path looks like a test file (by name/directory conventions). */
int is_test_file(const char *path);

/* Return 1 if path is a tracked source file type for TDD purposes. */
int is_tdd_source_file(const char *path);

/* Load session state from DB1 for the given session id. If no row exists,
 * defaults are applied. */
void session_state_load(session_state_t *state, const char *sid);

/* Persist session state to DB1 if dirty. No-op if sid is empty. */
void session_state_save(const session_state_t *state, const char *sid);

/* Persist session state to DB1 unconditionally (ignores the dirty flag). */
void session_state_force_save(const session_state_t *state, const char *sid);

/* Check if a command is a write/destructive operation. */
int is_write_command(const char *command);

/* Reload the external guardrail policy from disk on the next use.
 * Intended for tests and administrative refresh flows. */
void guardrails_policy_reset(void);

/* Check if a tool is a shell/bash provider. */
int is_shell_tool(const char *tool);

/* Parse a Bash command for a learnable workflow signal.
 *
 * Inspects the command for patterns that describe how a project is built,
 * tested, merged, or deployed (e.g. `gh pr create --base testing`, `make
 * test`, `npm test`). On match, writes a compact signal_type ("pr-target",
 * "test-command", "active-branch") and a human-readable rule sentence, then
 * returns 1. Returns 0 when no pattern matches.
 *
 * signal_type_out is bounded by stsize; rule_out by rsize. Both must be
 * non-NULL and non-zero length. */
int workflow_parse_bash_signal(const char *command, char *signal_type_out, size_t stsize,
                               char *rule_out, size_t rsize);

/* Normalize a relative path against cwd. Result written to buf. */
char *normalize_path(const char *path, const char *cwd, char *buf, size_t buf_len);

/* Resolve the git repository root for a directory. Returns 0 on success. */
int git_repo_root(const char *dir, char *out_root, size_t out_len);

/* Compute the expected repo-local aimee worktree path for a git repo and session.
 * If work_name is non-NULL, it disambiguates multiple worktrees within the
 * same session (e.g. parallel delegates).
 * Writes result to wt_buf. Returns 0 on success. */
int worktree_sibling_path(const char *git_root, const char *session_id, const char *work_name,
                          char *wt_buf, size_t wt_len);

/* Check if a path is already inside an aimee-managed worktree. */
int is_aimee_worktree_path(const char *path);

/* Create an aimee-managed worktree for a git repo. Returns 0 on success.
 * If work_name is non-NULL, creates a separate worktree for that work unit. */
int worktree_create_sibling(const char *git_root, const char *session_id, const char *work_name);

/* Clean up a session's worktree. Removes if clean, warns if dirty.
 * If work_name is non-NULL, targets the work-specific worktree. */
void worktree_cleanup(const char *git_root, const char *session_id, const char *work_name);

void worktree_registry_record(const char *git_root, const char *wt_path, const char *branch,
                              const char *sid, const char *work_name);
int worktree_find_branch_in_repo(const char *git_root, const char *branch, char *out_dir,
                                 size_t out_len);
int worktree_find_branch_registered(const char *branch, char *out_dir, size_t out_len);

/* Check if the branch in the given git directory has a merged PR. Returns 1 if merged.
 * If git_dir is NULL or empty, uses the current working directory. */
int check_merged_pr_for_branch(const char *git_dir);

/* Look up the worktree path for a given CWD from session state.
 * Returns the worktree path if the CWD is inside a tracked git root,
 * or NULL if no worktree applies. */
const char *worktree_for_cwd(const session_state_t *state, const char *cwd);

/* Copy uncommitted changes from a delegate worktree back to the parent worktree.
 * Returns count of files applied, 0 if nothing, -1 on argument error. */
int worktree_apply_changes_to_parent(const char *src_wt, const char *dst_wt);

/* Shared filesystem path validation for all tool paths.
 * Resolves symlinks, rejects traversal attempts, rejects sensitive paths.
 * Returns NULL on success (path is safe), or a static error string on failure.
 * On success, the resolved path is written to resolved_buf. */
const char *guardrails_validate_file_path(const char *path, char *resolved_buf,
                                          size_t resolved_len);

#endif /* DEC_GUARDRAILS_H */
