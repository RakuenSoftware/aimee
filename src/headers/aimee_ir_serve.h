/* aimee_ir_serve.h -- the live-path bridge (Slice 5): build an upstream provider
 * request from an inbound Anthropic /v1/messages request VIA THE IR, replacing the
 * legacy direct anthropic->openai translate_request. No client-shape -> provider-
 * shape path; the request pivots through the canonical IR. Wired into the ingress
 * behind a config flag, with legacy fallback until parity is proven live. */
#ifndef DEC_AIMEE_IR_SERVE_H
#define DEC_AIMEE_IR_SERVE_H 1

#include <stddef.h>

#include "aimee_ir.h" /* aimee_response_t */

struct cJSON;

/* Build the provider request body from `req` (an Anthropic Messages request) via
 * the IR, targeting the backend named by `driver_name` ("chatgpt" -> Responses,
 * else OpenAI chat). The served model is overridden to `agent_model` and, when
 * `max_tokens_override > 0`, the token cap is set to it (mirrors the legacy path's
 * agent shaping). Returns a malloc'd JSON string the caller frees, or NULL to fall
 * back to the legacy translator.
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

/* 1 if the IR live-path flag is enabled (config-only: AIMEE_IR_PATH env). */
int aimee_ir_path_enabled(void);

/* 1 if the IR-delta streaming relay is enabled (config-only: AIMEE_IR_STREAM_RELAY
 * env; DEFAULT-OFF, gated separately from AIMEE_IR_PATH). When on, the incremental
 * OpenAI-chat -> Anthropic SSE relay uses the neutral IR-delta model instead of the
 * legacy anthropic_stream_feed_openai translator. */
int aimee_ir_stream_relay_enabled(void);

/* Drop-in for openai_parse_responses_to_chat that routes the /v1/responses CLIENT
 * parse THROUGH THE IR (responses_frontend_parse -> IR -> chat components), instead
 * of a direct Responses->chat translation. Same out-param contract: `model` buffer,
 * malloc'd `*instructions_out`, and detached `*messages_out`/`*tools_out` cJSON the
 * caller owns; `*stream_out` mirrors the request. Returns 0, or -1 (caller falls
 * back to the legacy translator). */
int aimee_ir_responses_to_chat(const char *body, char *model, size_t model_n,
                               char **instructions_out, struct cJSON **messages_out,
                               struct cJSON **tools_out, int *stream_out);

/* Build a provider request from the agent path's chat components (messages + tools
 * + system) VIA THE IR, replacing driver->build_request's direct chat->provider
 * translation. Assembles a chat request, parses it to the IR, overrides the served
 * model, and builds for the backend named by `driver_name` ("chatgpt" -> Responses,
 * else OpenAI). Returns a new cJSON the caller owns, or NULL to fall back. */
struct cJSON *aimee_ir_build_from_chat(const char *agent_model, const struct cJSON *messages,
                                       const struct cJSON *tools, const char *system,
                                       const char *driver_name);

/* Slice 3 gate (AIMEE_IR_RESP_PATH env, DEFAULT-OFF): route the OPENAI-WIRE buffered
 * response parse through the IR. Per-wire; anthropic + responses stay on legacy. */
int aimee_ir_resp_path_enabled(void);

struct parsed_response;
/* TRANSITIONAL: IR response -> legacy parsed_response_t (see .c). Remove once emit +
 * police are IR-native. */
void aimee_ir_response_to_parsed(const aimee_response_t *r, struct parsed_response *out);

#endif /* DEC_AIMEE_IR_SERVE_H */
