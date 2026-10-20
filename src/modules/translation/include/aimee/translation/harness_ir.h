#ifndef AIMEE_TRANSLATION_HARNESS_IR_H
#define AIMEE_TRANSLATION_HARNESS_IR_H
#include <aimee/ir/session_event.h>
#include <aimee/translation/aimee_ir_stream.h>
typedef struct
{
   anthropic_backend_stream_state_t anthropic;
   struct aimee_harness_output *output;
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
   struct cJSON *acp_client_requests;
   aimee_block_t acp_client_call;
   const char *acp_session_id; /* Borrowed held native session, never the Aimee actor. */
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
/* Final model output since the last user/tool-result boundary. Ordered native
 * message groups remain typed; no text-to-IR fallback or history authority.
 * Requires complete admitted observations and leaves out unchanged on refusal. */
int aimee_harness_ir_response(const aimee_harness_ir_t *state, aimee_response_t *out);
/* Exact native client-resource intents use the acp namespace and native
 * read_text_file/write_text_file names and arguments. They grant no I/O. */
int aimee_acp_client_tool(const struct cJSON *frame, aimee_block_t *out);
int aimee_acp_take_client_tool(aimee_harness_ir_t *state, aimee_block_t *out);
int aimee_acp_client_tool_result_ir(aimee_harness_ir_t *state, const aimee_block_t *result);
struct cJSON *aimee_acp_error_reply(const struct cJSON *id, int code, const char *message);
struct cJSON *aimee_acp_client_tool_reply(const struct cJSON *frame, const aimee_block_t *result);
struct cJSON *aimee_acp_control_reply(const struct cJSON *id, const struct cJSON *request,
                                      const struct cJSON *decision);
void aimee_harness_ir_free(aimee_harness_ir_t *state);
#endif
