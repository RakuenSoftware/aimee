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
   struct aimee_harness_output *output;
   char message_id[256];
   unsigned long counter;
   struct cJSON *pending;
   aimee_harness_failure_t failure;
   int acp_open;
   int acp_prompt_id, acp_terminal_status, acp_text_seen, acp_external_tool_reports;
   aimee_block_type_t acp_kind;
   const char *acp_role;
   const char *acp_model; /* Borrowed selected external backend label, not SDK provenance. */
   struct cJSON *acp_client_requests;
   aimee_block_t acp_client_call;
   const char *acp_session_id; /* Borrowed held native session, never the Aimee actor. */
} aimee_harness_ir_t;
/* SDK controls/compaction end here; their original model/history observations
 * belong to the gateway. ACP has its own canonical external decoder. Returned
 * events borrow the frame only during the synchronous sink call. */
int aimee_claude_frame_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
int aimee_codex_frame_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
int aimee_codex_reply_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
int aimee_acp_frame_ir(const struct cJSON *frame, aimee_harness_ir_t *state);
/* Close authored output before a real producer-owned host request. The result
 * observer consumes an ALREADY committed actual result for the private business
 * view only; it never emits a second canonical message or grants execution. */
int aimee_acp_before_host_tool(aimee_harness_ir_t *state);
int aimee_acp_observe_host_tool_result(aimee_harness_ir_t *state, const aimee_block_t *result);
/* 0 pending, 1 matching protocol terminal, -1 matching peer error. Default
 * prompt id2 preserves the legacy protocol; the standard worker binds id3. */
int aimee_acp_turn_status(const aimee_harness_ir_t *state);
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
/* ACP model output since the last user/tool-result boundary. Ordered authored
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
