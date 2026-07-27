/* aimee_frontend.h -- FRONTEND adapters: the CLIENT wire protocol <-> the canonical
 * IR. parse = client request JSON -> aimee_request_t; render = aimee_response_t ->
 * client response JSON. Paired with the backend adapters (IR <-> provider wire), so
 * no client-shape -> provider-shape path ever exists. See aimee_ir.h + the proposal.
 *
 * These are built ALONGSIDE the legacy anthropic_ingress.c / openai_shape.c and are
 * wired into the live path behind a config flag in a later slice; the legacy
 * translators are deleted only once cross-protocol parity is proven live. */
#ifndef DEC_AIMEE_FRONTEND_H
#define DEC_AIMEE_FRONTEND_H 1

#include <stddef.h>

#include <aimee/ir/aimee_ir.h>

struct cJSON;

/* Parse an Anthropic Messages API request (/v1/messages) into the IR. Returns 0 on
 * success (out owned by caller -> aimee_request_free), -1 on error (err filled;
 * out zeroed). Sets out->frontend = AIMEE_WIRE_ANTHROPIC and keeps a whole-request
 * raw sidecar for same-protocol replay. */
int anthropic_frontend_parse(const struct cJSON *req, aimee_request_t *out, char *err, size_t errn);

/* Parse an OpenAI Chat Completions request (/v1/chat/completions) into the IR.
 * Sets out->frontend = AIMEE_WIRE_OPENAI_CHAT. */
int openai_frontend_parse(const struct cJSON *req, aimee_request_t *out, char *err, size_t errn);

/* Parse an OpenAI Responses API request (/v1/responses; codex client) into the IR.
 * Sets out->frontend = AIMEE_WIRE_RESPONSES. instructions -> system; input items
 * (message/function_call/function_call_output/reasoning) -> messages + typed blocks. */
int responses_frontend_parse(const struct cJSON *req, aimee_request_t *out, char *err, size_t errn);

/* Render an IR response as a buffered client response, built PURELY from the IR
 * (ruling Q3: semantic correctness, NOT provider byte-parity). Returns a new cJSON
 * object the caller owns (cJSON_Delete), or NULL on bad args. The canonical
 * stop_reason is mapped into the client's vocabulary; tool_use blocks render as the
 * client's tool-call shape (Anthropic input object / OpenAI arguments string from
 * the parsed tool_input). */
struct cJSON *anthropic_frontend_render(const aimee_response_t *resp);
struct cJSON *openai_frontend_render(const aimee_response_t *resp);
/* Render an IR response as an OpenAI Responses API response (output items). */
struct cJSON *responses_frontend_render(const aimee_response_t *resp);

#endif /* DEC_AIMEE_FRONTEND_H */
