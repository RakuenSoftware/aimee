/* aimee_backend.h -- BACKEND adapters: the canonical IR <-> the upstream PROVIDER
 * wire. build = aimee_request_t -> provider request JSON; parse = provider response
 * JSON -> aimee_response_t. Selected by the chosen backend model's provider,
 * INDEPENDENT of the frontend, so Claude Code (Anthropic frontend) can be served by
 * codex (Responses backend) with no direct Anthropic<->OpenAI code. Paired with the
 * frontend adapters (aimee_frontend.h). See the proposal. */
#ifndef DEC_AIMEE_BACKEND_H
#define DEC_AIMEE_BACKEND_H 1

#include <stddef.h>

#include "aimee_ir.h"

struct cJSON;

/* Build an Anthropic Messages API request from the IR. Returns a new cJSON object
 * the caller owns (cJSON_Delete), or NULL on bad args. */
struct cJSON *anthropic_backend_build(const aimee_request_t *ir);

/* Economizer caching gate for the Anthropic egress. The server sets this from the
 * economizer tier (off -> 0, disabling all aimee cache_control; safe/aggressive -> 1).
 * Default 1 (caching on). Kept as a runtime flag so this pure TU needs no config. */
void aimee_backend_anthropic_set_cache_enabled(int on);

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
/* Parse an OpenAI Responses API response (output items) into the IR. */
int responses_backend_parse(const struct cJSON *resp, aimee_response_t *out, char *err,
                            size_t errn);

#endif /* DEC_AIMEE_BACKEND_H */
