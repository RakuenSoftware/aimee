/* Canonical session content. Identity and ordering belong to the transport
 * envelope, never to a provider delta. This module has no storage or I/O. */
#ifndef AIMEE_IR_SESSION_EVENT_H
#define AIMEE_IR_SESSION_EVENT_H

#include <aimee/ir/aimee_ir.h>

typedef enum
{
   AIMEE_SESSION_DELTA = 0,
   AIMEE_SESSION_MESSAGE,
   AIMEE_SESSION_CONTROL_REQUEST,
   AIMEE_SESSION_CONTROL_RESPONSE,
   AIMEE_SESSION_EXECUTION
} aimee_session_event_kind_t;

typedef struct
{
   aimee_session_event_kind_t kind;
   const char *role;
   const char *message_id;
   const aimee_delta_t *delta;
   const aimee_message_t *message;
   /* Harness controls are not model content. request_id is opaque and must
    * match a pending server-owned request before any response is delivered. */
   const char *request_id;
   const char *control_kind;
   const struct cJSON *control;
   /* Host-task lifecycle is separate from each model response TURN_STOP. */
   const char *execution_status; /* running, completed, failed, cancelled */
   const char *execution_error;
} aimee_session_event_t;

/* Canonical JSON, independent of native protocols. Returned objects are owned
 * by the caller. Unknown/invalid typed payloads fail rather than become text.
 * Raw provider sidecars are deliberately not serialized into session events. */
struct cJSON *aimee_ir_block_json(const aimee_block_t *block);
struct cJSON *aimee_ir_message_json(const aimee_message_t *message);
struct cJSON *aimee_ir_delta_json(const aimee_delta_t *delta);
struct cJSON *aimee_ir_session_event_json(const aimee_session_event_t *event);

/* Thread-bound emission sink. Adapter boundaries publish typed IR here, not a
 * browser-specific event. A missing sink is legal for non-session consumers;
 * a failing installed sink is returned to the producer, never swallowed. */
typedef int (*aimee_ir_session_sink_fn)(const aimee_session_event_t *event, void *context);
void aimee_ir_session_set_sink(aimee_ir_session_sink_fn sink, void *context);
aimee_ir_session_sink_fn aimee_ir_session_get_sink(void **context);
int aimee_ir_session_emit(const aimee_session_event_t *event);
int aimee_ir_session_emit_response(const aimee_response_t *response, const char *message_id);

int aimee_ir_session_emit_tool_result(const char *tool_id, const char *result, int is_error);

#endif
