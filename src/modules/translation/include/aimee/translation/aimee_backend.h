/* aimee_backend.h -- BACKEND adapters: the canonical IR <-> the upstream PROVIDER
 * wire. build = aimee_request_t -> provider request JSON; parse = provider response
 * JSON -> aimee_response_t. Selected by the chosen backend model's provider,
 * INDEPENDENT of the frontend, so Claude Code (Anthropic frontend) can be served by
 * codex (Responses backend) with no direct Anthropic<->OpenAI code. Paired with the
 * frontend adapters (aimee_frontend.h). See the proposal. */
#ifndef DEC_AIMEE_BACKEND_H
#define DEC_AIMEE_BACKEND_H 1

#include <stddef.h>

#include <aimee/ir/aimee_ir.h>

struct cJSON;

/* Build an Anthropic Messages API request from the IR. Returns a new cJSON object
 * the caller owns (cJSON_Delete), or NULL on bad args. */
struct cJSON *anthropic_backend_build(const aimee_request_t *ir);

/* Parse an Anthropic Messages API response into the IR. Returns 0 (out owned by
 * caller -> aimee_response_free), -1 on error. */
int anthropic_backend_parse(const struct cJSON *resp, aimee_response_t *out, char *err,
                            size_t errn);

/* Build an OpenAI Chat Completions request from the IR (system blocks -> leading
 * system messages; tool_use -> assistant tool_calls; tools -> function tools).
 * Returns a new cJSON the caller owns, or NULL. */
struct cJSON *openai_backend_build(const aimee_request_t *ir);
/* Parse an OpenAI chat.completion response into the IR. */
int openai_backend_parse(const struct cJSON *resp, aimee_response_t *out, char *err, size_t errn);

/* Build an OpenAI Responses API request from the IR (codex): system blocks ->
 * `instructions`; messages -> `input` items (message / function_call); tools ->
 * flat function tools; max_tokens -> max_output_tokens. Returns a new cJSON. */
struct cJSON *responses_backend_build(const aimee_request_t *ir);
/* Reasoning text out of a Responses `reasoning` output item, from its `summary`.
 * SHARED by the backend (provider response) and frontend (client request) parsers,
 * which had independently assumed `summary` is a bare string.
 *
 * The shape is deliberately NOT assumed here. Responses carries a message's `content`
 * as an array of typed parts ({"type":"output_text","text":...}), and `summary`
 * follows the same idiom ({"type":"summary_text","text":...}) -- but a bare string
 * also appears. Both are accepted: a string is taken as-is, an array's part texts are
 * joined with blank lines, anything else yields NULL (no reasoning rather than an
 * empty thought). Read-side robustness only -- nothing about what aimee EMITS changes,
 * so being liberal here cannot put a wrong shape on the wire.
 *
 * Returns a malloc'd string the caller frees, or NULL. Pure. */
char *responses_reasoning_summary_text(const struct cJSON *item);

/* Parse an OpenAI Responses API response (output items) into the IR. */
int responses_backend_parse(const struct cJSON *resp, aimee_response_t *out, char *err,
                            size_t errn);

/* Build an AWS Bedrock Converse request from the IR (system blocks -> `system[]`
 * text parts; messages -> `messages[]` with toolUse/toolResult/reasoningContent
 * parts; tools -> `toolConfig.tools[].toolSpec`; sampling -> `inferenceConfig`).
 * The body is IDENTICAL for Converse and ConverseStream; modelId is a URI param and
 * is NOT emitted. Returns a new cJSON the caller owns, or NULL on bad args. */
struct cJSON *bedrock_converse_build(const aimee_request_t *ir);
/* Parse a Bedrock Converse response (output.message) into the IR. Returns 0 (out
 * owned by caller -> aimee_response_free), -1 + err on a missing/malformed message. */
int bedrock_converse_parse(const struct cJSON *resp, aimee_response_t *out, char *err, size_t errn);

/* Map a Converse `stopReason` string to the canonical stop-reason enum. Shared by
 * the non-streaming parse and the ConverseStream delta parser (messageStop) so there
 * is one source of truth. NULL / unrecognized -> AIMEE_STOP_UNKNOWN. */
aimee_stop_reason_t converse_stop_reason(const char *sr);

#endif /* DEC_AIMEE_BACKEND_H */
