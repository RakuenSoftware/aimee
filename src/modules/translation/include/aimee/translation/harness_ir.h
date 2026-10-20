#ifndef AIMEE_TRANSLATION_HARNESS_IR_H
#define AIMEE_TRANSLATION_HARNESS_IR_H
#include <aimee/ir/session_event.h>
#include <aimee/translation/aimee_ir_stream.h>
typedef struct
{
   anthropic_backend_stream_state_t anthropic;
   char message_id[256];
   unsigned long counter;
   struct cJSON *started;
   struct cJSON *pending;
   struct cJSON *claude_message;
   int claude_open;
   const char *admitted_prompt;
   int user_echo_seen;
   /* Original gateway capture owns model/tool observations. Native controls and
    * compaction remain observable; SDK-normalized copies never become history. */
   int host_model_capture;
   int acp_open;
   aimee_block_type_t acp_kind;
   const char *acp_role;
   struct cJSON *acp_tools;
} aimee_harness_ir_t;
/* Native frames end here. Returned events borrow into frame only during the
 * synchronous sink call. State owns only its lookup tables, not provider data. */
int aimee_claude_frame_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
int aimee_codex_frame_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
int aimee_codex_reply_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
int aimee_acp_frame_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
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
