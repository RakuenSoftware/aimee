/* aimee_frontend.h -- FRONTEND adapters: the CLIENT wire protocol <-> the canonical
 * IR. parse = client request JSON -> aimee_request_t; render = aimee_response_t ->
 * client response JSON. Paired with the backend adapters (IR <-> provider wire), so
 * no client-shape -> provider-shape path ever exists. See aimee_ir.h + the proposal.
 * Canonical admission is mandatory on the live ingress paths. */
#ifndef DEC_AIMEE_FRONTEND_H
#define DEC_AIMEE_FRONTEND_H 1

#include <stddef.h>

#include <aimee/ir/aimee_ir.h>
#include <aimee/translation/aimee_ir_stream.h>

struct cJSON;

/* A host may restore/refuse opaque state using its independently observed
 * provenance after native admission. The borrowed callback is thread-scoped;
 * restore the returned scope before releasing its context. Pure renderer
 * comparisons never invoke it or grant echoed state a new provenance. */
typedef int (*aimee_frontend_state_guard_fn)(aimee_request_t *request, void *context);
typedef struct
{
   aimee_frontend_state_guard_fn guard;
   void *context;
} aimee_frontend_state_scope_t;
aimee_frontend_state_scope_t aimee_frontend_bind_state_guard(aimee_frontend_state_guard_fn guard,
                                                             void *context);

/* An authenticated host execution may replace an SDK's proposed request with
 * its owned canonical context at ENTRY only, before replay-proof restoration.
 * This callback never runs at egress, so approved downstream transforms remain
 * effective. Scope/context are borrowed until explicitly restored. */
aimee_frontend_state_scope_t aimee_frontend_bind_request_owner(aimee_frontend_state_guard_fn owner,
                                                               void *context);

/* Recheck after host history restoration/transforms, immediately before the
 * actual provider renderer. Failure releases/zeros the owned request. */
int aimee_frontend_guard_state(aimee_request_t *request, char *error, size_t capacity);

/* Parse an exact native JSON frame and retain its owned byte representation. */
int aimee_frontend_parse_wire(const char *body, size_t length, aimee_wire_t wire,
                              aimee_request_t *out, char *error, size_t error_capacity);

/* Legacy text completion input enters directly as a canonical user block.
 * It has no provider-native raw replay form and never becomes a Chat request. */
int aimee_frontend_parse_text_completion(const char *body, size_t length, aimee_request_t *out,
                                         char *error, size_t capacity);

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
/* Caller owns history and report. A NULL history with a report explicitly
 * refuses model admission; tmux text is archival only, never inferred roles. */
struct cJSON *aimee_frontend_history_import_report(const char *raw, const char *provider,
                                                   const char *source_session,
                                                   const char *source_agent,
                                                   struct cJSON **out_report);

/* External Claude Code JSONL envelopes, already parsed into an array. Returns
 * canonical observations only: never host history, controls, effects or proofs.
 * Entire input must be supported, unique-keyed and bound to session_id. */
struct cJSON *aimee_frontend_claude_transcript_import(const struct cJSON *records,
                                                      const char *session_id);

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

/* Typed catalog entry/rendering boundaries, including namespace children and
 * explicitly provider-owned hosted-tool options. Parse outputs start zeroed;
 * release their owned fields with aimee_tool_free even after failed admission. */
int anthropic_ir_parse_tool(const struct cJSON *native, aimee_tool_t *tool);
int openai_ir_parse_tool(const struct cJSON *native, aimee_tool_t *tool, int nested);
struct cJSON *openai_ir_render_tool(const aimee_tool_t *tool, int responses_shape);

/* Shared OpenAI-family cache controls, parsed once into owned canonical fields.
 * Null uses the provider default; malformed types/ambiguous options refuse. */
int openai_family_prompt_cache_parse(const struct cJSON *native, aimee_request_t *out, char *error,
                                     size_t capacity);

/* Native client output boundaries. The response is borrowed canonical IR;
 * rendering never mutates or consumes it. Responses tail excludes the caller's
 * single response.created envelope. Emitters return 0 on success, -1 on failure. */
typedef void (*aimee_chat_emit_fn)(void *context, const char *data_json);
struct cJSON *aimee_openai_render_response(const aimee_response_t *response, const char *id,
                                           const char *model, long created, int responses);
int aimee_openai_buffered_response(const aimee_response_t *response, const char *id,
                                   const char *model, long created, int responses, char *buffer,
                                   int capacity);
int aimee_openai_emit_chat_response(const aimee_response_t *response, const char *id,
                                    const char *model, long created, aimee_chat_emit_fn emit,
                                    void *context);
int aimee_openai_emit_responses_tail(const aimee_response_t *response, const char *id,
                                     const char *model, long created, aimee_sse_emit_fn emit,
                                     void *context);

/* Shared owned text/media content adapter for Chat request/response boundaries. */
int openai_ir_parse_content(const struct cJSON *content, aimee_block_t **blocks, int *count);

/* Ordered Chat text/media at either request or response output boundary. */
struct cJSON *openai_ir_render_content(const aimee_message_t *message);

/* Shared typed output-format entry: Chat nests json_schema, Responses does not. */
int openai_family_output_format_parse(const struct cJSON *format, int responses_shape,
                                      aimee_request_t *out, char *error, size_t capacity);

#endif /* DEC_AIMEE_FRONTEND_H */
