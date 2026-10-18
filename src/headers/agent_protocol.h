/* agent_protocol.h: LLM request building and response parsing */
#ifndef DEC_AGENT_PROTOCOL_H
#define DEC_AGENT_PROTOCOL_H 1

#include "agent_types.h"
#include <aimee/ir/aimee_ir.h>

struct cJSON;

/* --- Parsed response types --- */

typedef struct
{
   char id[64];
   /* Was [32], which silently TRUNCATED. A namespaced MCP tool name routinely
    * exceeds it: "mcp__aimee__preview_blast_radius" is exactly 32 characters, so
    * it could not fit even with its terminator. */
   char name[128];
   /* The tool's owning namespace, empty when it has none.
    *
    * A Codex client sends its MCP tools inside a `namespace` group, and the
    * provider answers with the nested name BARE plus this field beside it:
    *
    *   {"type":"function_call","name":"git","namespace":"mcp__aimee",...}
    *
    * The client routes on the pair, so dropping this half makes the call
    * unroutable -- it answers "unsupported call: git". Modeled rather than left
    * to the raw sidecar for the same reason as aimee_block_t.thinking_signature:
    * the sidecar is same-protocol replay, and this has to survive the chat hop in
    * the middle of responses -> IR -> chat -> IR -> responses. */
   char tool_namespace[64];
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
    * captured for the audit. Empty when absent. Its vocabulary is whatever the
    * provider (or a policy stage) wrote, so never compare it against one wire's
    * words to decide behaviour -- use `stop`. */
   char stop_reason[32];
   /* The canonical stop reason, carried from the IR parse. AIMEE_STOP_UNKNOWN (the
    * zero value) when no IR parse produced this response. Emitters render it in
    * their own client's vocabulary. */
   aimee_stop_reason_t stop;
} parsed_response_t;

/* --- Request builders --- */

struct cJSON *agent_build_request_openai(const agent_t *agent, struct cJSON *messages,
                                         struct cJSON *tools, int max_tokens, double temperature);

struct cJSON *agent_build_request_responses(const agent_t *agent, struct cJSON *input,
                                            struct cJSON *tools, const char *system_prompt);

struct cJSON *agent_build_request_anthropic(const agent_t *agent, struct cJSON *messages,
                                            struct cJSON *tools, const char *system_prompt,
                                            int max_tokens, double temperature);

/* The runtime stores canonical {role,blocks} messages. Native history is accepted
 * only by this migration adapter, and native requests are built only at egress. */
struct cJSON *agent_build_request_ir(const agent_t *agent, const struct cJSON *messages,
                                     struct cJSON *tools, const char *system_prompt, int max_tokens,
                                     double temperature, aimee_wire_t wire);

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

/* Resolve the output-token cap for an outbound request: an explicit caller
 * `requested` (>0) wins, else the agent's pinned max_tokens (>0), else the
 * model's registry ceiling (model_max_output). Never returns 0 — request
 * builders emit a concrete, model-appropriate cap rather than a hardcoded one. */
int agent_request_max_tokens(const agent_t *agent, int requested);

/* --- Response parsers --- */

/* Extract the Responses (codex) response object (the one with the `output` array)
 * from an SSE body, so the IR responses_backend_parse can consume it. Returns a new
 * cJSON the caller owns, or NULL. */
struct cJSON *agent_responses_sse_response_object(const char *body);

/* Native response entry adapters. The caller owns the full typed response until
 * tool dispatch and history append finish. No text/tool projection occurs here.
 * rescue_mode < 0 skips rescue, 0 admits dialect calls, 1 also admits bare JSON;
 * n_rescued receives the admitted recovered-call count when provided. */
#define AGENT_IR_CAPTURE_FAILED (-2)
struct aimee_anthropic_replay;
int agent_ir_decode_json_response_bound(const struct cJSON *root, int anthropic, int rescue_mode,
                                        int *n_rescued,
                                        const struct aimee_anthropic_replay *request,
                                        aimee_response_t *out);
int agent_ir_decode_json_response(const struct cJSON *root, int anthropic, int rescue_mode,
                                  int *n_rescued, aimee_response_t *out);
int agent_ir_decode_responses(const char *body, int rescue_mode, int *n_rescued,
                              aimee_response_t *out);

/* Legacy output-only compatibility values are released here while their
 * remaining native emitters are retired. Execution never constructs them. */
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
