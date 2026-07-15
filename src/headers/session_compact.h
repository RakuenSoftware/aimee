/* session_compact.h: structured session compaction for long-running conversations.
 *
 * When a session approaches the model's context window, this module summarises
 * the older portion of the message history into a compact boundary marker and
 * preserves the most-recent turns verbatim so the agent can continue without
 * losing immediate context.
 *
 * Summary boundary structure:
 *   [CONTEXT COMPACTION — REFERENCE ONLY]
 *   ## Active Task
 *   ## Goal
 *   ## Constraints
 *   ## Completed Actions
 *   ## Active State
 *   ## In Progress
 *   ## Blocked
 *   ## Key Decisions
 *   ## Resolved Questions
 *   ## Pending User Asks
 *   ## Relevant Files
 *   [turn N+1 onward retained verbatim]
 *
 * Pressure levels (returned by session_compact_pressure):
 *   SESSION_PRESSURE_OK      — context usage is fine
 *   SESSION_PRESSURE_WARN    — approaching limit, consider preparing
 *   SESSION_PRESSURE_COMPACT — compact now before the next LLM call
 */
#ifndef SESSION_COMPACT_H
#define SESSION_COMPACT_H 1

struct cJSON;

/* Pressure levels */
#define SESSION_PRESSURE_OK      0
#define SESSION_PRESSURE_WARN    1
#define SESSION_PRESSURE_COMPACT 2

/* Default thresholds (percentage of context window) */
#define SESSION_COMPACT_DEFAULT_WARN_PCT    70
#define SESSION_COMPACT_DEFAULT_COMPACT_PCT 80

/* Number of recent messages always retained verbatim (the "tail") */
#define SESSION_COMPACT_RETAIN_TAIL 6

/* Maximum length of the generated summary text */
#define SESSION_COMPACT_SUMMARY_MAX 8192

/* Pre-boundary read-only tool signatures captured for the rounds-to-resume
 * metric (see docs/proposals/pending/compaction-quality-measurement.md).
 *
 * Why capture here: compaction DELETES the summarised messages, so the set of
 * tool calls the agent had already made is destroyed by the very operation we
 * want to measure. It cannot be recovered afterwards from any surviving table —
 * it has to be read out before the delete, or not at all.
 *
 * These are reported, not persisted: session_compact() stays a pure function of
 * the messages array, and the caller decides whether the numbers are worth
 * storing. */
#define SESSION_COMPACT_MAX_SIGS 64
#define SESSION_COMPACT_SIG_LEN  64

typedef struct
{
   int warn_pct;    /* warn threshold %; 0 = use default (70) */
   int compact_pct; /* compact threshold %; 0 = use default (80) */
   int retain_tail; /* recent messages to keep verbatim; 0 = use default (6) */
} session_compact_config_t;

typedef struct
{
   int compacted;        /* 1 if compaction happened; 0 if not needed/messages too few */
   int messages_before;  /* total messages before compaction */
   int messages_after;   /* total messages after compaction */
   int messages_removed; /* messages replaced by the summary boundary */
   int repairs;          /* tool-pair repairs performed before compaction */
   char summary[SESSION_COMPACT_SUMMARY_MAX]; /* generated summary text */

   /* Read-only tool signatures ("<name>:<16-hex FNV-1a of arguments>") found in
    * the messages about to be discarded, deduplicated. A post-boundary call
    * matching one of these re-derives state the agent already had — that is the
    * rounds-to-resume signal.
    *
    * Only tools whose repetition can ONLY mean lost context are recorded; see
    * compact_tool_is_readonly() for the list and why it is not the `readonly`
    * toolset. */
   int readonly_sig_count;    /* entries in readonly_sigs */
   int readonly_sigs_dropped; /* distinct sigs beyond the cap */
   char readonly_sigs[SESSION_COMPACT_MAX_SIGS][SESSION_COMPACT_SIG_LEN];
} session_compact_result_t;

/* Estimate the approximate token count for a cJSON messages array.
 * Uses a 4-character-per-token approximation on serialised content.
 * Returns 0 if messages is NULL or not an array. */
int session_compact_estimate_tokens(struct cJSON *messages);

/* Check context pressure given token counts and context window size.
 * Returns SESSION_PRESSURE_OK / WARN / COMPACT.
 * prompt_tokens + completion_tokens: accumulated this session.
 * context_window: model limit in tokens (0 = unknown; returns OK).
 * cfg may be NULL to use built-in defaults. */
int session_compact_pressure(int prompt_tokens, int completion_tokens, int context_window,
                             const session_compact_config_t *cfg);

/* Compact a cJSON messages array in-place.
 *
 * Steps performed:
 *   1. Repair orphaned tool call / result pairs (message_history_repair).
 *   2. Build a structured text summary of the messages to be discarded.
 *   3. Insert a "compaction boundary" user message with the summary.
 *   4. Remove the summarised messages, retaining the tail verbatim.
 *   5. Merge consecutive same-role messages (messages_compact_consecutive).
 *
 * The first message is always kept (typically the system prompt or original
 * task). Messages in the middle are summarised.  The last `retain_tail`
 * messages are kept verbatim.
 *
 * Returns 0 on success (result->compacted indicates whether anything changed).
 * Returns -1 on error (messages NULL or not an array).
 * cfg may be NULL to use built-in defaults.
 * out may be NULL if the caller only needs the side effect. */
int session_compact(struct cJSON *messages, const session_compact_config_t *cfg,
                    session_compact_result_t *out);

/* Compute the read-only signature ("<name>:<16-hex FNV-1a of args>") for a
 * single tool call.
 *
 * Returns 1 and fills `out` if `name` is a tool whose repetition can ONLY mean
 * the agent lost context (see the list in session_compact.c and why it is not
 * toolset.c's `readonly` set). Returns 0 and leaves `out` untouched otherwise —
 * mutating and verifying tools (edit_file, test, bash, verify, ...) are legitimate
 * to repeat and must never be counted as re-derivation.
 *
 * Exposed so the post-boundary comparator computes signatures the same way the
 * capture does; the tool list and hash live in exactly one place. */
int session_compact_tool_sig(const char *name, const char *args, char *out, size_t out_len);

/* Produce a structured 11-section summary for a given focus topic without a
 * real messages array.  Builds a minimal synthetic conversation so that
 * ## Active Task is set to `topic`.  All other sections are populated with
 * default placeholder text.
 *
 * Output is written into `out` (NUL-terminated).
 * Returns 0 on success, -1 on error (topic NULL or out NULL). */
int session_compact_focused(const char *topic, char *out, size_t out_len);

#endif /* SESSION_COMPACT_H */
