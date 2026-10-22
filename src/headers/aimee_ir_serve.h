/* Canonical native-wire ingress/build seams. Admission is mandatory;
 * disabling host context edits never selects a legacy provider builder. */
#ifndef DEC_AIMEE_IR_SERVE_H
#define DEC_AIMEE_IR_SERVE_H 1

#include <stddef.h>

#include <aimee/ir/aimee_ir.h> /* aimee_response_t */

struct cJSON;

/* Build the provider request body from `req` (an Anthropic Messages request) via
 * the IR, targeting the backend named by `driver_name` ("chatgpt" -> Responses,
 * else OpenAI chat). The served model is overridden to `agent_model` and, when
 * `max_tokens_override > 0`, the token cap is set to it (mirrors the legacy path's
 * agent shaping). Returns a malloc'd JSON string the caller frees, or NULL on admission/build
 * failure.
 *
 * `want_stream` decides the upstream stream flag EXPLICITLY rather than inheriting
 * the client's. They are not the same question: the caller may serve the client an
 * SSE stream while fetching the upstream reply BUFFERED and replaying it. Passing
 * the client's flag through caused exactly that bug — the buffered-replay path
 * asked the provider to stream and then cJSON_Parse'd the SSE ("primary provider
 * returned an unparseable reply"). */
char *aimee_ir_build_provider_body(const struct cJSON *req, const char *driver_name,
                                   const char *agent_model, int max_tokens_override,
                                   int want_stream);

/* Apply the configured host stages to an already admitted canonical request. */
void aimee_ir_apply_gateway_request_stages(aimee_request_t *request);

/* Native Anthropic retains its configured context-injection opt-in while using
 * the same canonical stage catalog as translated requests. */
void aimee_ir_apply_native_request_stages(aimee_request_t *request);

/* Compatibility predicate: canonical request admission is always enabled. */
int aimee_ir_path_enabled(void);

/* Compatibility predicate: canonical incremental stream relay is always enabled. */
int aimee_ir_stream_relay_enabled(void);

/* Flat-text persona delivery for the legacy (non-IR) chat path. Claims the
 * once-per-session delivery, prepends when this is a first turn, and releases the
 * claim. Returns NULL when the claim state is unreadable so the caller keeps the
 * original text and stays retry-eligible. */
char *aimee_ir_prepend_active_persona_text(const char *text, int conversation_established);

#endif /* DEC_AIMEE_IR_SERVE_H */
