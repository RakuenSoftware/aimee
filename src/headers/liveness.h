/* liveness.h: delegate liveness detection utilities.
 *
 * Pure functions for detecting stuck, idle, or silently-failed delegates.
 * These are used by the agent loop to surface liveness failures explicitly
 * rather than letting them look like success or silent timeout.
 *
 * Signals monitored:
 *   1. Empty or whitespace-only final responses.
 *   2. Repeated identical tool calls (circuit breaker).
 *   3. Write-capable agents that keep retrieving without making progress.
 *
 * Thresholds:
 *   LIVENESS_REPEAT_WARN_THRESHOLD  — inject warning after N identical calls in a row.
 *   LIVENESS_REPEAT_ABORT_THRESHOLD — hard abort after N warning injections total.
 */

#ifndef DEC_LIVENESS_H
#define DEC_LIVENESS_H 1

#include <stddef.h>

/* Number of consecutive identical tool calls before a warning is injected. */
#define LIVENESS_REPEAT_WARN_THRESHOLD 3

/* Number of warning injections before the circuit breaker trips and the loop aborts. */
#define LIVENESS_REPEAT_ABORT_THRESHOLD 3

/* A write-capable delegate gets an action checkpoint after this many successful
 * non-mutating calls, even when every retrieval was unique. */
#define LIVENESS_PROGRESS_INITIAL_CALLS 12

/* After a checkpoint, this many further successful non-mutating calls escalate
 * the warning. The third ignored checkpoint terminates the loop. */
#define LIVENESS_PROGRESS_FOLLOWUP_CALLS 8

/* Two retrievals that duplicate or overlap an item still in the sliding window
 * are enough to bring the initial checkpoint forward. */
#define LIVENESS_PROGRESS_DUPLICATE_HITS 2

#define LIVENESS_PROGRESS_WINDOW 12
#define LIVENESS_PROGRESS_TOOL_MAX 64
#define LIVENESS_PROGRESS_TARGET_MAX 256

typedef enum
{
   LIVENESS_PROGRESS_NONE = 0,
   LIVENESS_PROGRESS_CHECKPOINT = 1,
   LIVENESS_PROGRESS_ESCALATE = 2,
   LIVENESS_PROGRESS_ABORT = 3
} liveness_progress_action_t;

typedef struct
{
   char tool[LIVENESS_PROGRESS_TOOL_MAX];
   char target[LIVENESS_PROGRESS_TARGET_MAX];
   int range_start;
   int range_end;
   int has_range;
} liveness_progress_retrieval_t;

typedef struct
{
   liveness_progress_retrieval_t window[LIVENESS_PROGRESS_WINDOW];
   int window_count;
   int window_next;
   int calls_since_mutation;
   int calls_since_checkpoint;
   int duplicate_hits;
   int checkpoints;
   int successful_mutations;
} liveness_progress_state_t;

/* Initialize a sliding-window progress guard. */
void liveness_progress_init(liveness_progress_state_t *state);

/* Observe one completed tool call. `target` is the stable retrieval identity
 * (normally a path, query, or canonical arguments). Ranges are inclusive.
 * Unsuccessful calls do not advance the guard. A successful mutation clears
 * all no-progress evidence. */
liveness_progress_action_t
liveness_progress_observe(liveness_progress_state_t *state, const char *tool, const char *target,
                          int range_start, int range_end, int has_range, int successful,
                          int mutation);

/* Format the model-visible instruction associated with CHECKPOINT or ESCALATE. */
void liveness_format_progress_hint(const liveness_progress_state_t *state,
                                   liveness_progress_action_t action, char *buf, size_t bufsz);

/* Returns 1 if content is NULL, empty, or contains only whitespace characters.
 * A delegate that returns such a response has not produced usable output. */
int liveness_is_empty_response(const char *content);

/* Returns 1 if content has no usable semantic signal, such as punctuation-only
 * model degeneration or long low-diversity numeric loops. This is stricter
 * than empty-response detection and is used to avoid treating local-provider
 * garbage as a successful delegate result. */
int liveness_is_degenerate_response(const char *content);

/* Returns 1 if content looks like an unexecuted tool-use plan: future-tense
 * prose plus fenced shell commands, but no actual findings. Callers should
 * only treat this as fatal when the delegate recorded zero tool calls. */
int liveness_is_unexecuted_tool_plan_response(const char *content);

/* Reject a degenerate owned response before it is recorded as successful output.
 * On rejection, frees *response, clears it, writes error_message into error, and
 * returns 1. Returns 0 when the response has usable semantic signal. */
int liveness_reject_degenerate_response(char **response, char *error, size_t error_sz,
                                        const char *error_message);

/* Fill buf with a human-readable failure diagnostic for an empty delegate response.
 * agent_name may be NULL. */
void liveness_format_empty_diagnostic(const char *agent_name, char *buf, size_t bufsz);

/* Fill buf with a final-response diagnostic when the loop has disabled tools
 * for the forced final turn but the model still attempts a tool call.
 * partial_text may contain any useful text the model emitted before the tool
 * call marker. */
void liveness_format_final_tool_call_diagnostic(const char *agent_name, const char *attempted_tool,
                                                const char *partial_text,
                                                const char *last_tool_name,
                                                const char *last_tool_result, int total_tool_calls,
                                                char *buf, size_t bufsz);

/* Returns 1 if the circuit breaker should trip.
 * total_triggers: number of warning injections so far (incremented by caller).
 * Callers should increment before calling, so the first warning = trigger 1. */
int liveness_circuit_breaker_tripped(int total_triggers);

typedef enum
{
   LIVENESS_FINAL_RESPONSE_NONE = 0,
   LIVENESS_FINAL_RESPONSE_SOFT = 1,
   LIVENESS_FINAL_RESPONSE_HARD = 2
} liveness_final_response_mode_t;

/* Classify whether the delegate loop should push for a final response.
 * SOFT means a role-specific final-after policy fired while normal turns remain.
 * HARD means the loop is on the last usable turn before max_turns is exhausted. */
liveness_final_response_mode_t
liveness_final_response_mode(int turn, int max_turns, int total_tool_calls, int final_after_turns);

/* Soft and hard final-response modes both disable tools. Soft mode is the
 * role-specific early final threshold; hard mode is the last usable turn. */
int liveness_final_response_allows_tools(liveness_final_response_mode_t mode);

#endif /* DEC_LIVENESS_H */
