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

/* Parse an exact native JSON frame and retain its owned byte representation. */
int aimee_frontend_parse_wire(const char *body, size_t length, aimee_wire_t wire,
                              aimee_request_t *out, char *error, size_t error_capacity);

/* Admit host-adjusted native fields with an immutable original frame sidecar.
 * Typed transforms run after this boundary; semantic comparison gates exact replay. */
int aimee_frontend_parse_native_frame(const struct cJSON *native, const char *frame, size_t length,
                                      aimee_wire_t wire, aimee_request_t *out, char *error,
                                      size_t capacity);

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

/* Parse the OpenAI-family tool_choice and parallel_tool_calls request fields into the
 * typed IR. `tool_choice` is the request's value (NULL/null = unset); responses_shape
 * selects the named-function object spelling ({type:function,name} on Responses,
 * {type:function,function:{name}} on Chat). Returns 0 on success, -1 for a choice the
 * IR cannot express (err filled), -2 on allocation failure. Shared by the Chat and
 * Responses frontends, which spell everything else identically. */
int openai_family_tool_choice_parse(const struct cJSON *tool_choice,
                                    const struct cJSON *parallel_tool_calls, int responses_shape,
                                    aimee_request_t *out, char *err, size_t errn);

/* Import a complete legacy native history at its entry boundary, or validate an
 * already canonical {role,blocks} history. Returns an owned canonical array;
 * NULL on malformed/mixed shapes. Lifted legacy system blocks are excluded:
 * the runtime supplies its current authoritative system separately. */
struct cJSON *aimee_frontend_history_import(const struct cJSON *messages, aimee_wire_t legacy_wire);

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

/* Typed OpenAI-family catalog entry/rendering boundaries, including namespace
 * children and explicitly provider-owned hosted-tool options. */
int openai_ir_parse_tool(const struct cJSON *native, aimee_tool_t *tool, int nested);
struct cJSON *openai_ir_render_tool(const aimee_tool_t *tool, int responses_shape);

/* Shared owned text/media content adapter for Chat request/response boundaries. */
int openai_ir_parse_content(const struct cJSON *content, aimee_block_t **blocks, int *count);

/* Ordered Chat text/media at either request or response output boundary. */
struct cJSON *openai_ir_render_content(const aimee_message_t *message);

/* Shared typed output-format entry: Chat nests json_schema, Responses does not. */
int openai_family_output_format_parse(const struct cJSON *format, int responses_shape,
                                      aimee_request_t *out, char *error, size_t capacity);

#endif /* DEC_AIMEE_FRONTEND_H */
