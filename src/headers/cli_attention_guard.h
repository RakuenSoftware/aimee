/* cli_attention_guard.h: P3 attention-weighted destructive guard (§D/§E).
 *
 * A Claude Code PreToolUse hook (`aimee attention-guard`) that keeps a compact
 * per-session attention log — which files the session has read/edited, decayed
 * by recency — and HARD-BLOCKS a destructive operation (rm -rf, truncate, shred,
 * `: > file`, …) on a file the session has paid real attention to. This catches
 * the "agent deletes the file it has been editing" class of mistake.
 *
 * Hook-protocol note: Claude Code's PreToolUse supports block (exit 2, stderr
 * shown to the model) or allow (exit 0) — there is no non-blocking "warn" tier,
 * so the guard only blocks the hardest-destructive ops on high-attention files
 * and otherwise allows. It complements (does not replace) the git-write guard
 * and the blast-radius skill; it is recency/attention-weighted and op-level.
 *
 * The scoring, op classification, and path extraction are pure and unit-tested;
 * the JSON log persistence + the hook entry live in the .c.
 */
#ifndef DEC_CLI_ATTENTION_GUARD_H
#define DEC_CLI_ATTENTION_GUARD_H 1

#include <stddef.h>

/* Op class for a tool call. */
typedef enum
{
   ATTN_OP_READ = 0,    /* read / non-destructive — accrue attention only */
   ATTN_OP_SOFT = 1,    /* edit/write/overwrite/rm (non-recursive) */
   ATTN_OP_HARD = 2,    /* rm -rf, truncate, shred, dd, mkfs, `: > f` — blockable */
   ATTN_OP_RAW_SCAN = 3 /* recursive raw grep/read; nudge toward Aimee tools */
} attn_op_t;

/* One recorded action against a path. */
typedef struct
{
   const char *path;
   int weight; /* kind weight: edit=8, read=2, … */
   long ts;    /* unix seconds */
} attn_record_t;

/* Recency-decayed attention score for `path`: sum of weight * 2^(-age_hours)
 * over records matching `path`. Pure. */
double attn_score(const attn_record_t *recs, int n, const char *path, long now_ts);

/* The attention threshold at/above which a path counts as "high attention"
 * (one edit, or two reads, within the hour). */
#define ATTN_HIGH_THRESHOLD 2.0

/* Classify a tool call. `bash_cmd` is the command string for the Bash tool (may
 * be NULL for non-Bash tools). Pure. */
attn_op_t attn_classify(const char *tool_name, const char *bash_cmd);

/* True when the tool call is a raw recursive exploration attempt that should use
 * Aimee's indexed tools first. Pure. */
int attn_is_raw_scan(const char *tool_name, const char *bash_cmd);

/* The attention weight to record for a tool call of the given class. Pure. */
int attn_weight_for(attn_op_t op);

/* Session-isolation decision (pure, testable). Returns 1 to BLOCK a mutating op
 * (ATTN_OP_SOFT/HARD) whose effective target is NOT inside an aimee-managed
 * worktree, else 0. The effective target is the absolute `file_path` when given
 * (Edit/Write), otherwise `cwd` (a relative file_path resolves under cwd; a Bash
 * mutation runs there). Read/raw-scan ops are never blocked. Used by
 * handle_attention_guard only when require_session_worktree is enabled. */
int attn_session_isolation_blocked(attn_op_t op, const char *file_path, const char *cwd);

/* External-memory decision (pure, testable). Returns 1 to BLOCK a tool call
 * that would WRITE an external file-based agent-memory store
 * (~/.claude/projects/<slug>/memory/...): a mutating file tool whose target
 * resolves into the store, or a Bash command that names the store with write
 * intent (redirect, in-place editor, file tool, or interpreter). Reads are
 * never blocked. Durable memories belong in aimee's memory system
 * (`aimee memory store`). Enforced by handle_attention_guard unless aimee.yaml
 * sets `require_aimee_memory: false` (default ON; no env-var bypass). */
int attn_external_memory_blocked(attn_op_t op, const char *tool_name, const char *file_path,
                                 const char *bash_cmd, const char *cwd);

/* `aimee attention-guard` PreToolUse-hook entry. Reads the host hook JSON from
 * stdin, updates the per-session attention log, and returns the hook exit code
 * (2 = block a hard-destructive op on a high-attention file, or — only when a
 * positive ingress_max_raw_scans cap is configured — block a raw recursive scan
 * once that per-session cap is exhausted; 0 = allow). With no cap configured
 * (the default) raw scans are never blocked. Never blocks on read/soft ops.
 * There is no env-var bypass; disabling it is an operator config action. */
int handle_attention_guard(void);

/* Authoritative "is session-worktree isolation required?" check (default ON
 * unless aimee.yaml sets `require_session_worktree: false`). Exposed so the
 * remote/thin session-start path can gate its worktree-prep directive on the
 * same answer the guard uses to block. */
int attn_require_session_worktree(void);

#endif /* DEC_CLI_ATTENTION_GUARD_H */
