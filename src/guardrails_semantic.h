/* guardrails_semantic.h: semantic risk scoring interface (Phase 0/1).
 *
 * Advises the deterministic guardrail policy with a multi-head risk score
 * produced by an external sidecar. The sidecar is invoked only after all
 * deterministic checks pass and only for write-intent tools.
 *
 * Default posture: enabled=false, dry_run=true — the sidecar runs but its
 * output is logged only and never changes the allow/block outcome. */
#ifndef DEC_GUARDRAILS_SEMANTIC_H
#define DEC_GUARDRAILS_SEMANTIC_H 1

#include "cJSON.h"
#include "headers/config.h"
#include <stddef.h>

/* Byte caps for bounded sidecar inputs (per proposal). */
#define GSEM_PATHS_CAP  2048
#define GSEM_CMD_CAP    4096
#define GSEM_DIFF_CAP   1024
#define GSEM_TASK_CAP   256
#define GSEM_EXPL_CAP   512
#define GSEM_LABELS_CAP 256

/* Bounded input assembled from pre_tool_check context. */
typedef struct
{
   char tool_name[64];
   char cwd[1024];
   char session_mode[32];
   char paths[GSEM_PATHS_CAP];
   char shell_cmd[GSEM_CMD_CAP];
   char old_excerpt[GSEM_DIFF_CAP];
   char new_excerpt[GSEM_DIFF_CAP];
   char active_task[GSEM_TASK_CAP];
} gsem_input_t;

/* Parsed sidecar response. parse_ok=0 means the sidecar failed or returned
 * invalid JSON — caller should treat as "allow" (deterministic-only fallback). */
typedef struct
{
   double overall;
   double action_risk;
   double diff_risk;
   double drift_risk;
   double antipattern_similarity;
   char recommendation[16];
   char labels[GSEM_LABELS_CAP];
   char explanation[GSEM_EXPL_CAP];
   int parse_ok;
} gsem_output_t;

/* Build bounded input from the pre_tool_check arguments.
 * Extracts paths, shell excerpt, diff excerpts from input_json. */
void gsem_build_input(const char *tool_name, cJSON *input_json, const char *cwd,
                      const char *session_mode, gsem_input_t *out);

/* Invoke the sidecar via |command| (sh -c).
 * Writes JSON to stdin; parses JSON from stdout.
 * On spawn/parse failure: sets out->parse_ok=0, out->recommendation="allow". */
int gsem_assess(const gsem_input_t *in, const char *command, gsem_output_t *out);

/* Map score to policy action per thresholds.
 * Returns "allow", "warn", "prompt", or "block".
 * Returns "allow" when parse_ok=0 (degrade to deterministic). */
const char *gsem_policy(const gsem_output_t *out, double warn_t, double prompt_t, double block_t);

/* Apply the promoted guardrail_strictness arm to semantic thresholds.
 * The default/balanced arm leaves config unchanged. */
void gsem_apply_strictness_arm(config_t *cfg);

/* Format the user-visible Phase 2 advisory warning for warn/prompt bands.
 * Returns 1 when |action| was formatted, 0 when no advisory applies. */
int gsem_format_advisory_message(char *buf, size_t buf_len, const char *action,
                                 const gsem_output_t *out);

/* Write a guardrail_event row to DB1.
 * No-op when session_id is NULL or empty. */
void gsem_record(const char *session_id, const gsem_input_t *in, const gsem_output_t *out,
                 const char *final_action, int dry_run);

/* Sidecar failure backoff: after a failure, gsem_assess() skips the sidecar
 * for GSEM_BACKOFF_MS milliseconds and logs only the first failure per
 * backoff interval.  Returns 1 when in backoff (sidecar not invoked), 0 when
 * the sidecar was reached. */
#define GSEM_BACKOFF_MS 30000

/* Check and update the failure backoff state.
 * Returns 1 when the sidecar is in cooldown (caller should skip invocation),
 * 0 when the sidecar can be called.  Must be called before gsem_assess. */
int gsem_sidecar_in_backoff(void);

/* Reset the backoff state.  Called on successful sidecar response, and by
 * tests to avoid state leakage. */
void gsem_sidecar_reset_backoff(void);

#endif /* DEC_GUARDRAILS_SEMANTIC_H */
