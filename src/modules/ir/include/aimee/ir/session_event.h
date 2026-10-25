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
   AIMEE_SESSION_EXECUTION,
   AIMEE_SESSION_PROJECTION,
   AIMEE_SESSION_CHILD_EXECUTION,
   AIMEE_SESSION_REPLAY_PROOF,
   AIMEE_SESSION_NATIVE_CHECKPOINT,
   AIMEE_SESSION_NATIVE_COMPACTION
} aimee_session_event_kind_t;

/* Host-only completed native cache observation. Contains commitments and an
 * opaque handle, never a duplicate transcript or credential. */
typedef struct
{
   const char *scope, *context_digest, *prefix_digest;
   const char *harness, *native_id, *claim;
   int message_count;
} aimee_native_checkpoint_t;

typedef struct
{
   aimee_session_event_kind_t kind;
   const char *role;
   const char *message_id;
   const aimee_delta_t *delta;
   const aimee_message_t *message;
   /* Host source provenance; never part of model payload/checkpoint content. */
   const struct cJSON *native_owner;
   int message_partial;
   /* Harness controls are not model content. request_id is opaque and must
    * match a pending server-owned request before any response is delivered. */
   const char *request_id;
   const char *control_kind;
   const struct cJSON *control;
   /* Host-task lifecycle is separate from each model response TURN_STOP. */
   const char *execution_status; /* running, completed, failed, cancelled */
   const char *execution_error;
   /* Host-only joined verifier receipts committed atomically with a completed
    * terminal. Exact bounded metadata, never model content or grade authority. */
   const struct cJSON *verification_proofs;
   /* Private execution reference; its conversation remains in its own log. */
   const char *child_session, *child_turn, *child_role;
   const struct cJSON *child_learning_binding; /* host-owned job/registration snapshot */
   /* Output-boundary fidelity report; never model content. */
   const char *projection_model, *projection_boundary;
   int projection_reasoning, projection_opaque, projection_cache_controls;
   int projection_host_context_blocks_as_text, projection_unverified_external_tool_reports;
   int projection_history_messages_as_text;
   /* Host-observed native state. Digests and the actual sent-prefix binding
    * contain no credential or duplicate transcript. Never model content. */
   const char *replay_scope, *replay_digest, *replay_wire, *replay_binding;
   const aimee_native_checkpoint_t *native_checkpoint;
   const char *native_harness; /* native compaction invalidates a derived cache only */
} aimee_session_event_t;

/* Canonical JSON, independent of native protocols. Returned objects are owned
 * by the caller. Unknown/invalid typed payloads fail rather than become text.
 * Raw provider sidecars are deliberately not serialized into session events. */
struct cJSON *aimee_ir_block_json(const aimee_block_t *block);
struct cJSON *aimee_ir_message_json(const aimee_message_t *message);
/* Exact owned response-cache snapshots, separate from session-event payloads.
 * Includes opaque adapter sidecars. Decode validates typed response content and
 * atomically replaces out only on success; neither path authorizes execution. */
struct cJSON *aimee_ir_response_json(const aimee_response_t *response);
int aimee_ir_response_parse(const struct cJSON *json, aimee_response_t *out);
/* Canonical request/catalog snapshot: no native replay bytes or raw sidecars. */
struct cJSON *aimee_ir_request_json(const aimee_request_t *request);
/* Strict owned inverse. Replaces initialized out only after complete admission;
 * native raw sidecars are deliberately not part of canonical request snapshots. */
int aimee_ir_request_parse(const struct cJSON *json, aimee_request_t *out);
/* Bounded identity/query projection for request policy, never a model prompt. */
struct cJSON *aimee_ir_policy_view_json(const aimee_request_t *request);
/* Decode canonical persisted content, never a provider protocol. On failure out
 * is empty; on success it owns its fields (aimee_message_free). */
int aimee_ir_message_parse(const struct cJSON *json, aimee_message_t *out);
/* Clone canonical stored messages without accepting or guessing a provider wire.
 * NULL denotes an empty conversation. The caller owns the returned array. */
struct cJSON *aimee_ir_history_clone(const struct cJSON *messages);
/* Repair a compatibility view's tool cycles in order; -1 leaves input untouched.
 * Missing results become explicit platform diagnostics with unknown outcomes,
 * never cancellation/effect receipts. Authoritative loaders reject any repair. */
int aimee_ir_history_repair(struct cJSON *messages);
/* Read-only admission of authoritative model history. Reject incomplete/orphan
 * cycles and synthetic unresolved-result diagnostics; no effect is authorized. */
/* Explicit host-consumer seam. Generic admission always refuses reserved
 * lifecycle diagnostics. Installed host callback must verify durable custody. */
typedef int (*aimee_ir_host_history_admission_fn)(const struct cJSON *messages);
void aimee_ir_history_set_host_admission(aimee_ir_host_history_admission_fn admit);
int aimee_ir_history_admit_host(const struct cJSON *messages);
int aimee_ir_history_admit(const struct cJSON *messages);
struct cJSON *aimee_ir_delta_json(const aimee_delta_t *delta);
struct cJSON *aimee_ir_session_event_json(const aimee_session_event_t *event);
/* Structural metadata admission only; resource custody/grade policy is external. */
int aimee_ir_verification_proof_valid(const struct cJSON *proof);

/* Flat MCP form data and normalized operator replies. No transport or I/O. */
int aimee_ir_form_validate(const struct cJSON *schema, const struct cJSON *content);
int aimee_ir_elicitation_url_valid(const struct cJSON *url);
int aimee_ir_control_reply_valid(const char *kind, const struct cJSON *request,
                                 const struct cJSON *reply);

/* Thread-bound emission sink. Adapter boundaries publish typed IR here, not a
 * browser-specific event. A missing sink is legal for non-session consumers;
 * a failing installed sink is returned to the producer, never swallowed. */
typedef int (*aimee_ir_session_sink_fn)(const aimee_session_event_t *event, void *context);
void aimee_ir_session_set_sink(aimee_ir_session_sink_fn sink, void *context);
/* Independent worker task boundary: discard an earlier task's failure only
 * when there is no active parent capture owner to preserve. */
void aimee_ir_session_begin_task(void);
/* Restore a nested execution owner without clearing its capture failure. */
void aimee_ir_session_restore_sink(aimee_ir_session_sink_fn sink, void *context, int failed);
aimee_ir_session_sink_fn aimee_ir_session_get_sink(void **context);
int aimee_ir_session_emit(const aimee_session_event_t *event);
int aimee_ir_session_emit_projection_controls(const char *model, int reasoning, int opaque,
                                              int cache_controls, const char *boundary);
int aimee_ir_session_emit_projection(const char *model, int reasoning, int opaque,
                                     const char *boundary);
/* Capture failure is sticky for the installed task sink, including adapter
 * admission failures. Clearing the sink ends the scope and resets the latch. */
void aimee_ir_session_fail_capture(void);
int aimee_ir_session_capture_failed(void);
/* Snapshot for a response whose native stream already supplies lifecycle. */
int aimee_ir_session_emit_response(const aimee_response_t *response, const char *message_id);
/* Buffered response: TURN_START, complete typed message, TURN_STOP with usage. */
int aimee_ir_session_emit_buffered_response(const aimee_response_t *response,
                                            const char *message_id);

int aimee_ir_session_emit_tool_result(const char *tool_id, const char *result, int is_error);

#endif
