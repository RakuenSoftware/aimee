/* agent_protocol.h: LLM request building and response parsing */
#ifndef DEC_AGENT_PROTOCOL_H
#define DEC_AGENT_PROTOCOL_H 1

#include "agent_types.h"

struct cJSON;

/* --- Parsed response types --- */

typedef struct
{
   char id[64];
   char name[32];
   char *arguments; /* malloc'd */
} parsed_tool_call_t;

/* Named tag so low-level headers (e.g. openai_shape.h) can forward-declare
 * `parsed_response_t` by pointer without including this agent-pipeline header. */
typedef struct parsed_response
{
   int is_tool_call;
   char *content; /* malloc'd, NULL if tool call */
   parsed_tool_call_t calls[AGENT_MAX_TOOL_CALLS];
   int call_count;
   struct cJSON *assistant_message; /* cloned, caller frees */
   int prompt_tokens;
   int completion_tokens;
   int cache_write_tokens; /* Anthropic: cache_creation_input_tokens */
   int cache_read_tokens;  /* Anthropic: cache_read_input_tokens */
   /* Provider-reported model id echoed in the response (often more specific than
    * the requested/served alias, e.g. a dated version). Empty when absent. */
   char model[MAX_MODEL_LEN];
   /* Provider stop/finish reason (Anthropic stop_reason / OpenAI finish_reason),
    * captured for the audit. Empty when absent. */
   char stop_reason[32];
} parsed_response_t;

/* --- Request builders --- */

struct cJSON *agent_build_request_openai(const agent_t *agent, struct cJSON *messages,
                                         struct cJSON *tools, int max_tokens, double temperature);

struct cJSON *agent_build_request_responses(const agent_t *agent, struct cJSON *input,
                                            struct cJSON *tools, const char *system_prompt);

struct cJSON *agent_build_request_anthropic(const agent_t *agent, struct cJSON *messages,
                                            struct cJSON *tools, const char *system_prompt,
                                            int max_tokens, double temperature);

/* Set the Anthropic `system` field on an outbound request (§3 cache-aware
 * shaping). When cache_marking is non-zero, the STABLE prefix (the text before
 * the volatile per-turn <aimee-context> envelope) is emitted as a cache_control
 * content block while the volatile context is left uncached; otherwise a plain
 * string is emitted. The min_chars floor is applied to the actual cacheable
 * STABLE prefix (not the whole prompt): when that prefix is shorter than
 * min_chars (>0), the request is left uncached. Adds nothing when system_prompt
 * is empty. Anthropic-only: callers build Anthropic-format requests, so this
 * never reaches a non-Anthropic provider. */
void agent_anthropic_set_system(struct cJSON *req, const char *system_prompt, int cache_marking,
                                int min_chars);

/* Build a Gemini generateContent request.
 * cache_name: "cachedContents/..." from gemini_prompt_cache_create(), or "" for uncached. */
struct cJSON *agent_build_request_gemini(const agent_t *agent, struct cJSON *messages,
                                         struct cJSON *tools, const char *system_prompt,
                                         int max_tokens, double temperature,
                                         const char *cache_name);

/* Resolve the output-token cap for an outbound request: an explicit caller
 * `requested` (>0) wins, else the agent's pinned max_tokens (>0), else the
 * model's registry ceiling (model_max_output). Never returns 0 — request
 * builders emit a concrete, model-appropriate cap rather than a hardcoded one. */
int agent_request_max_tokens(const agent_t *agent, int requested);

/* --- Response parsers --- */

void agent_parse_response_openai(struct cJSON *root, parsed_response_t *out);
void agent_parse_response_responses(const char *body, parsed_response_t *out);

/* Extract the Responses (codex) response object (the one with the `output` array)
 * from an SSE body, so the IR responses_backend_parse can consume it. Returns a new
 * cJSON the caller owns, or NULL. */
struct cJSON *agent_responses_sse_response_object(const char *body);
void agent_parse_response_anthropic(struct cJSON *root, parsed_response_t *out);

/* Parse a provider JSON response through the canonical IR and bridge it into a
 * parsed_response_t (the sole response parser for the JSON wires).
 * `anthropic` selects the anthropic vs openai backend parser. `rescue_mode` gates the
 * XML tool-call rescue the parser owns: <0 skips it, 0 rescues dialect calls but not
 * bare prose JSON, 1 also rescues bare JSON. `*n_rescued` (if non-NULL) receives how
 * many calls the rescue recovered. Returns 0 on success, -1 if the IR could not parse
 * (caller falls back to the legacy translators). */
int agent_ir_parse_json_response(struct cJSON *root, int anthropic, int rescue_mode, int *n_rescued,
                                 parsed_response_t *out);

/* Build an OpenAI assistant message ({role, content:null, tool_calls}) from parsed
 * calls -- used to make a replayable turn after an XML tool-call rescue. */
struct cJSON *agent_build_openai_assistant_message_from_calls(parsed_response_t *parsed);
/* Parse a Gemini generateContent response. Tracks cachedContentTokenCount as cache_read_tokens. */
void agent_free_parsed_response(parsed_response_t *p);

/* --- Message utilities --- */

int messages_compact_consecutive(struct cJSON *messages);

/* Repair inconsistent message history before sending to LLM.
 * - Orphaned tool calls (no matching result) get synthetic cancellation results.
 * - Orphaned tool results (no matching call) are removed.
 * - Trailing unanswered tool calls are filled.
 * Handles OpenAI (role=tool, tool_call_id) and Anthropic (tool_use/tool_result, tool_use_id).
 * Returns the number of repairs performed. Idempotent. */
int message_history_repair(struct cJSON *messages);

#endif /* DEC_AGENT_PROTOCOL_H */
