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

/* Explicit wire boundary; unchanged same-protocol requests retain exact bytes.
 * Independent semantic comparison detects edits even when their flag was missed. */
char *aimee_backend_render_wire(const aimee_request_t *request, aimee_wire_t wire);
/* Completed JSON/SSE entry boundary. Owns parsing and canonical admission; no
 * fallback to legacy parsers or projection occurs. */
int aimee_backend_parse_wire(const char *frame, size_t length, aimee_wire_t wire,
                             aimee_response_t *out, char *error, size_t capacity);
/* Shared Anthropic block entry adapter used by request/response boundaries.
 * Unknown native types retain UNKNOWN + sidecar; canonical admission rejects
 * them until an explicit typed mapping exists. */
void anthropic_ir_parse_block(const struct cJSON *native, aimee_block_t *out);

typedef struct
{
   int omitted_reasoning_blocks;
   int omitted_opaque_state_blocks;
   int omitted_cache_controls;
} aimee_wire_projection_report_t;
/* Reports wire incompatibility without editing canonical history. */
void aimee_backend_projection_report(const aimee_request_t *request, aimee_wire_t wire,
                                     aimee_wire_projection_report_t *report);
int anthropic_ir_block_replayable(const aimee_block_t *block);

/* A local replay fence from the actual sent Anthropic request. No credentials or
 * provider state; signatures remain opaque and are still verified upstream. */
typedef struct aimee_anthropic_replay
{
   char prefix[65], chain[65];
   int valid;
} aimee_anthropic_replay_t;
int aimee_anthropic_bind_request(const struct cJSON *native, aimee_request_t *request);
int aimee_anthropic_replay_prepare(const void *bytes, size_t length,
                                   aimee_anthropic_replay_t *state);
int aimee_anthropic_bind_response(const aimee_anthropic_replay_t *request,
                                  aimee_response_t *response);
int aimee_anthropic_replay_valid(const aimee_request_t *request, const struct cJSON *native);
void aimee_anthropic_projection_report(const aimee_request_t *request,
                                       aimee_wire_projection_report_t *report);

/* Render one admitted block at an explicit Anthropic output boundary. */
struct cJSON *anthropic_ir_render_block(const aimee_block_t *block);

/* Prepare owned host system blocks and their scalar/block cache representation.
 * Cache only the stable prefix before <aimee-context, subject to min_chars.
 * Requires fresh Anthropic IR without system/native sidecars; atomic on error. */
int anthropic_ir_set_host_system(aimee_request_t *request, const char *system, int cache_marking,
                                 int min_chars);

/* Build an Anthropic Messages API request from the IR. Returns a new cJSON object
 * the caller owns (cJSON_Delete), or NULL on bad args. */
struct cJSON *anthropic_backend_build(const aimee_request_t *ir);

/* Plain host-turn compatibility layout. Values still come entirely from IR;
 * sampling follows content, and system_last preserves the existing cache layout. */
struct cJSON *anthropic_backend_build_plain(const aimee_request_t *request, int system_last);

/* Parse an Anthropic Messages API response into the IR. Returns 0 (out owned by
 * caller -> aimee_response_free), -1 on error. */
int anthropic_backend_parse(const struct cJSON *resp, aimee_response_t *out, char *err,
                            size_t errn);

/* Host-created OpenAI-compatible settings with no canonical cross-wire meaning.
 * Translation owns their native representation. Negative sampling values omit;
 * fallbacks are borrowed and are deduplicated after the selected IR model. */
typedef struct
{
   double min_p, repeat_penalty;
   int disable_thinking, split_reasoning;
   const char *const *fallback_models;
   int n_fallback_models;
} aimee_openai_host_profile_t;
/* Requires a fresh Chat IR request without native entry sidecars. Atomic on error. */
int openai_ir_set_host_profile(aimee_request_t *request,
                               const aimee_openai_host_profile_t *profile);

/* Build an OpenAI Chat Completions request from the IR (system blocks -> leading
 * system messages; tool_use -> assistant tool_calls; tools -> function tools).
 * Returns a new cJSON the caller owns, or NULL. */
struct cJSON *openai_backend_build(const aimee_request_t *ir);
/* Plain host-turn wire layout: typed sampling follows the rendered content. */
struct cJSON *openai_backend_build_plain(const aimee_request_t *request);
/* Add the typed IR tool choice and parallel_tool_calls to an OpenAI-family request
 * object. responses_shape selects {type:function,name} (Responses) over
 * {type:function,function:{name}} (Chat). Shared by the Chat and Responses backends. */
void openai_family_add_tool_choice(struct cJSON *out, const aimee_request_t *ir,
                                   int responses_shape);
/* Emit only the typed cache controls. Returns -1 on allocation failure. */
int openai_family_add_prompt_cache(struct cJSON *out, const aimee_request_t *request);
/* Parse an OpenAI chat.completion response into the IR. */
int openai_backend_parse(const struct cJSON *resp, aimee_response_t *out, char *err, size_t errn);

/* Build an OpenAI Responses API request from the IR (codex): system blocks ->
 * `instructions`; messages -> `input` items (message / function_call); tools ->
 * flat function tools; max_tokens -> max_output_tokens. Returns a new cJSON. */
struct cJSON *responses_backend_build(const aimee_request_t *ir);
/* Canonical history -> stable Codex thread/inject_items output boundary.
 * Returns owned Responses input items, preserving roles/order/full tool IDs.
 * System/tools/sampling stay with their separate host configuration owners.
 * Caller proves resume prefix and account scope; upstream replay guard still
 * validates opaque reasoning when these items reach the selected model. */
struct cJSON *aimee_codex_history_items(const aimee_request_t *history);
/* Owned native snapshot assembled at the Responses SSE entry boundary. */
struct cJSON *responses_backend_sse_object(const char *body);
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

/* Explicit output boundary for the typed format; NULL when unset or invalid. */
struct cJSON *openai_family_output_format_build(const aimee_output_format_t *format,
                                                int responses_shape);

#endif /* DEC_AIMEE_BACKEND_H */
