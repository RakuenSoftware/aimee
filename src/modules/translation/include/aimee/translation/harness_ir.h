#ifndef AIMEE_TRANSLATION_HARNESS_IR_H
#define AIMEE_TRANSLATION_HARNESS_IR_H
#include <aimee/ir/session_event.h>
/* Safe native diagnostic categories, never provider text or retry authority. */
typedef enum
{
   AIMEE_HARNESS_FAILURE_NONE,
   AIMEE_HARNESS_FAILURE_RATE_LIMIT,
   AIMEE_HARNESS_FAILURE_PROVIDER_REJECTED
} aimee_harness_failure_t;
typedef struct
{
   struct cJSON *pending;
   aimee_harness_failure_t failure;
} aimee_harness_ir_t;
/* SDK controls/compaction end here; their original model/history observations
 * belong to the gateway. Returned
 * events borrow the frame only during the synchronous sink call. */
int aimee_claude_frame_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
int aimee_codex_frame_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
int aimee_codex_reply_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
/* Canonical operator decision -> documented native control output boundary.
 * Answers are {question-id:[selected/free-form strings]}; original tool input
 * is retained unless that tool explicitly asks the operator for answers. */
struct cJSON *aimee_claude_control_reply(const char *native_id, const struct cJSON *request,
                                         const struct cJSON *decision);
struct cJSON *aimee_codex_control_reply(const char *method, const struct cJSON *params,
                                        const struct cJSON *decision);
/* Persist an already-admitted canonical operator decision before native delivery.
 * It resolves the pending adapter id, so native serialization emits no duplicate. */
int aimee_harness_control_decision_ir(aimee_harness_ir_t *state, const char *native_id,
                                      const struct cJSON *decision);
void aimee_harness_ir_free(aimee_harness_ir_t *state);
#endif
