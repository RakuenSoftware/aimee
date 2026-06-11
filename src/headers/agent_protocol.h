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

typedef struct
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

/* Build a Gemini generateContent request.
 * cache_name: "cachedContents/..." from gemini_prompt_cache_create(), or "" for uncached. */
struct cJSON *agent_build_request_gemini(const agent_t *agent, struct cJSON *messages,
                                         struct cJSON *tools, const char *system_prompt,
                                         int max_tokens, double temperature,
                                         const char *cache_name);

/* --- Response parsers --- */

void agent_parse_response_openai(struct cJSON *root, parsed_response_t *out);
void agent_parse_response_responses(const char *body, parsed_response_t *out);
void agent_parse_response_anthropic(struct cJSON *root, parsed_response_t *out);
/* Parse a Gemini generateContent response. Tracks cachedContentTokenCount as cache_read_tokens. */
void agent_parse_response_gemini(struct cJSON *root, parsed_response_t *out);
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
