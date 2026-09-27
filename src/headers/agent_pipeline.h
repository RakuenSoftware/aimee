/* agent_pipeline.h: autonomous end-to-end pipeline (autopilot) */
#ifndef AGENT_PIPELINE_H
#define AGENT_PIPELINE_H

#include "aimee.h"
#include "cJSON.h"

/* --- Phase names --- */
#define PIPE_PHASE_CLASSIFY "classify"
#define PIPE_PHASE_CLARIFY  "clarify"
#define PIPE_PHASE_PLAN     "plan"
#define PIPE_PHASE_EXECUTE  "execute"
#define PIPE_PHASE_QA       "qa"
#define PIPE_PHASE_VALIDATE "validate"
#define PIPE_PHASE_DONE     "done"

/* --- Status names --- */
#define PIPE_STATUS_ACTIVE    "active"
#define PIPE_STATUS_PAUSED    "paused"
#define PIPE_STATUS_COMPLETE  "complete"
#define PIPE_STATUS_FAILED    "failed"
#define PIPE_STATUS_CANCELLED "cancelled"

/* --- Circuit-breaker attempt limits per phase --- */
#define PIPELINE_MAX_ATTEMPTS_PLAN     3
#define PIPELINE_MAX_ATTEMPTS_EXECUTE  5
#define PIPELINE_MAX_ATTEMPTS_QA       5
#define PIPELINE_MAX_ATTEMPTS_VALIDATE 3
#define PIPELINE_MAX_ATTEMPTS_CLARIFY  8

/* --- pipeline_advance() return codes --- */
#define PIPELINE_ADV_PROGRESSED 0  /* still in progress, call again later */
#define PIPELINE_ADV_DONE       1  /* pipeline reached 'complete' */
#define PIPELINE_ADV_PAUSED     -1 /* circuit-breaker tripped, needs human input */
#define PIPELINE_ADV_ERROR      -2 /* pipeline not found or in terminal state */

/* --- Buffer size constants --- */
#define PIPELINE_MAX_MSG_LEN 4096

/* --- Public API --- */

/* Classify a task for pipeline creation.
 * plan_depth may be NULL/empty or one of: trivial, simple, complex. */
void pipeline_classify_task(const char *task, const char *plan_depth, char *out, size_t out_len);

/* Advance a pipeline one step.  Writes a human-readable guidance message into
 * msg (up to msg_len bytes).  Returns one of the PIPELINE_ADV_* codes. */
int pipeline_advance(int pipeline_id, char *msg, size_t msg_len);

/* Write a multi-line status report into buf (up to buf_len bytes).
 * Returns the number of bytes written, or -1 if the pipeline was not found. */
int pipeline_status_report(int pipeline_id, char *buf, size_t buf_len);

/* List up to limit active+paused pipelines into buf.
 * Returns the count of rows written. */
int pipeline_list(char *buf, size_t buf_len, int limit);

/* MCP tool handler (action: start|advance|status|list|cancel|resume|
 *                           link-plan|link-job).
 * Pipelines are stored in DB1 (db1_pipeline_*); no KB_STORE dependency.
 * Returns a malloc'd JSON string; caller frees. */
char *handle_autopilot(cJSON *args);

#endif /* AGENT_PIPELINE_H */
