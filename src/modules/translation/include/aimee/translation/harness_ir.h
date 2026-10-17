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
void aimee_harness_ir_free(aimee_harness_ir_t *state);
#endif
