#ifndef DEC_AGENT_RUNTIME_MESSAGES_H
#define DEC_AGENT_RUNTIME_MESSAGES_H 1

#include "cJSON.h"
#include <stddef.h>

#define AGENT_FINAL_TOOL_RETRY_LIMIT          2
#define AGENT_DEGENERATE_RESPONSE_RETRY_LIMIT 1

int agent_session_retry_final_tool_violation(cJSON *messages, const char *attempted_action,
                                             int *turn, int *max_t, int initial_max_t,
                                             int *retry_count, char *error, size_t error_len);
int agent_session_retry_degenerate_response(cJSON *messages, int *turn, int *retry_count,
                                            int *force_text_only_retry);
void agent_session_append_final_message(cJSON *messages, const char *content);
void agent_session_append_final_instruction(cJSON *messages);
void agent_session_append_final_retry_instruction(cJSON *messages, const char *attempted_action);
void agent_session_append_degenerate_retry_instruction(cJSON *messages);

#endif /* DEC_AGENT_RUNTIME_MESSAGES_H */
