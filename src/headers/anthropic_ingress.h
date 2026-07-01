/* anthropic_ingress.h: inbound Anthropic Messages API translation.
 *
 * aimee-server exposes POST /v1/messages so Claude Code (and any Anthropic
 * Messages API client) can drive aimee's configured primary model. The proxy
 * is a stateless wire-format translator: it does NOT route through aimee's
 * agent loop, memory, or toolset. The client owns its system prompt, message
 * history, and tools, and tool execution stays client-side; aimee only swaps
 * the model transport and re-encodes the wire format.
 *
 * This module holds the pure (cJSON-only) translation between the Anthropic
 * Messages wire format and the OpenAI chat/completions format aimee's
 * provider drivers speak. The anthropic provider driver is a near-passthrough
 * (the inbound body is already Anthropic-shaped); the OpenAI-family drivers
 * (minimax, mistral, mimo, openai, ...) need the conversions below. */
#ifndef DEC_ANTHROPIC_INGRESS_H
#define DEC_ANTHROPIC_INGRESS_H 1

#include "aimee.h"          /* size macros consumed transitively by agent_types.h */
#include "agent_protocol.h" /* parsed_response_t */

struct cJSON;

/* Flatten an Anthropic request's "system" field — which may be a plain string
 * or an array of {type:"text", text:...} blocks — into a single newly
 * malloc'd string (text blocks joined with "\n\n"). Returns NULL when no
 * system prompt is present or it is empty. Caller frees. */
char *anthropic_system_to_text(const struct cJSON *req);

/* Convert an Anthropic "messages" array into an OpenAI chat/completions
 * "messages" array (new cJSON array, caller owns via cJSON_Delete). Content
 * blocks map as:
 *   text         -> assistant/user text
 *   image        -> OpenAI image_url part (base64 data URL)
 *   tool_use     -> assistant message tool_calls[] entry (arguments stringified)
 *   tool_result  -> a {role:"tool", tool_call_id, content} message
 * When system_text is non-NULL/non-empty it is prepended as a system message.
 * Returns NULL on malformed input (messages not an array). */
struct cJSON *anthropic_messages_to_openai(const struct cJSON *messages, const char *system_text);

/* Convert an Anthropic "tools" array ([{name,description,input_schema}]) into
 * an OpenAI tools array ([{type:"function",function:{name,description,
 * parameters}}]). Returns a new array (caller owns), or NULL when there are no
 * usable tools. */
struct cJSON *anthropic_tools_to_openai(const struct cJSON *anthropic_tools);

/* Convert Anthropic tools to the OpenAI Responses API's flat function-tool
 * shape: [{type:"function",name,description,parameters}]. */
struct cJSON *anthropic_tools_to_responses(const struct cJSON *anthropic_tools);

/* Build a non-streaming Anthropic Messages API response object from a parsed
 * provider reply. resp_id is the "msg_..." id; model echoes the request's
 * model string. Produces:
 *   {id,type:"message",role:"assistant",model,
 *    content:[{type:"text",...}|{type:"tool_use",...}],
 *    stop_reason,stop_sequence:null,usage:{input_tokens,output_tokens}}
 * stop_reason is "tool_use" when the reply contains tool calls, else
 * "end_turn". Caller owns the returned object. */
struct cJSON *anthropic_response_from_parsed(const char *resp_id, const char *model,
                                             const parsed_response_t *parsed);

/* --- Streaming translation (OpenAI chat chunks -> Anthropic Messages SSE) ---
 *
 * Claude Code always streams /v1/messages. The translator consumes the
 * provider's OpenAI-style `chat.completion.chunk` `data:` payloads and emits
 * the Anthropic SSE event sequence:
 *   message_start
 *   (content_block_start, content_block_delta*, content_block_stop)*  // text + tool_use
 *   message_delta   (stop_reason + output usage)
 *   message_stop
 * The emit callback matches server_http.c's sse_event_emit: it receives the
 * event name and the JSON data payload; the caller owns framing/transport. The
 * translator is pure (no I/O) so it is unit-testable with synthetic chunks. */
typedef void (*anthropic_sse_emit_fn)(void *ctx, const char *event, const char *data_json);

typedef struct anthropic_stream_xlate anthropic_stream_xlate_t;

/* Begin a stream: allocates state and emits message_start immediately (so the
 * client sees a first event without waiting for the model). input_tokens is the
 * prompt-token estimate for message_start.usage; pass 0 if unknown. Returns
 * NULL on allocation failure. */
anthropic_stream_xlate_t *anthropic_stream_begin(const char *msg_id, const char *model,
                                                 int input_tokens, anthropic_sse_emit_fn emit,
                                                 void *ctx);

/* Feed one provider SSE `data:` payload (the text after "data: "). Accepts the
 * OpenAI chat.completion.chunk shape; "[DONE]", NULL, or unparseable lines are
 * ignored (terminal closure happens in anthropic_stream_finish). */
void anthropic_stream_feed_openai(anthropic_stream_xlate_t *st, const char *data_json);

/* Close any open content block and emit message_delta + message_stop. */
void anthropic_stream_finish(anthropic_stream_xlate_t *st);

/* Replay a fully-parsed provider reply (`anthropic_response_from_parsed`
 * equivalent) as a well-formed Anthropic SSE sequence. Used by the streaming
 * /v1/messages paths when the gateway response-side tool-policing policy
 * (`gateway_prevent_subagents`) is active: the upstream is buffered to
 * completion, the police function compacts `parsed.calls[]`, and this helper
 * replays the policed struct as if the per-block translator had been
 * streaming. Pure (no I/O); deterministic; unit-testable with a captured
 * emit. Mirrors `anthropic_response_from_parsed`'s wire shape:
 *   message_start (with usage: input_tokens, cache_creation_input_tokens,
 *                  cache_read_input_tokens if non-zero)
 *   one text content_block_start/.../stop pair — always emitted, even when
 *                  parsed.content is empty/NULL, matching the buffered
 *                  renderer's empty-text-block fallback (lines 427-433 of
 *                  src/server/anthropic_ingress.c). Index 0.
 *   one tool_use content_block_start + (input_json_delta)* + stop per
 *                  surviving parsed.calls[0..call_count-1] entry. Indices
 *                  1, 2, ... in original order.
 *   message_delta (with parsed.stop_reason verbatim — see the police
 *                  function's contract in gateway_policy.h — and usage
 *                  output_tokens + cache_read_input_tokens if non-zero).
 *   message_stop.
 * `emit` matches the streaming translator's emit signature
 * (`anthropic_sse_emit_fn`). `ctx` is the caller's transport context
 * (passed through to `emit`). */
void emit_message_as_sse(const parsed_response_t *parsed, const char *msg_id, const char *model,
                         anthropic_sse_emit_fn emit, void *ctx);

/* Read the usage tapped from the upstream OpenAI stream: the prompt count
 * (upstream-reported if seen, else the begin-time estimate), the completion
 * count, and any cached prompt tokens. Any out pointer may be NULL. Lets the
 * caller write an accurate ingress cost row for the OpenAI-via-translator path. */
void anthropic_stream_get_usage(const anthropic_stream_xlate_t *st, int *input_tokens,
                                int *output_tokens, int *cache_read_tokens);

/* Free translator state (does not emit). */
void anthropic_stream_free(anthropic_stream_xlate_t *st);

/* --- Per-request passthrough headers (parity mode) ---
 *
 * Claude Code sends `anthropic-version` and `anthropic-beta` headers that select
 * API behavior (wire version, beta features such as fine-grained tool streaming
 * or 1h cache TTL). The /v1/messages ingress handlers receive only the body, so
 * server_http captures these two headers per request and the anthropic-driver
 * passthrough forwards them upstream when the primary speaks the Anthropic API. Set on
 * every request (pass "" / NULL to clear); the getters return "" when unset. */
void anthropic_ingress_set_request_headers(const char *anthropic_version,
                                           const char *anthropic_beta);
const char *anthropic_ingress_request_version(void);
const char *anthropic_ingress_request_beta(void);

#endif /* DEC_ANTHROPIC_INGRESS_H */
