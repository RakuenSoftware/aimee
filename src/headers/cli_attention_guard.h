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

/* `aimee attention-guard` PreToolUse-hook entry. Reads the host hook JSON from
 * stdin, updates the per-session attention log, and returns the hook exit code
 * (2 = block a hard-destructive op on a high-attention file, or — only when a
 * positive ingress_max_raw_scans cap is configured — block a raw recursive scan
 * once that per-session cap is exhausted; 0 = allow). With no cap configured
 * (the default) raw scans are never blocked. Never blocks on read/soft ops. Set
 * AIMEE_GUARD=0 to bypass. */
int handle_attention_guard(void);

#endif /* DEC_CLI_ATTENTION_GUARD_H */
