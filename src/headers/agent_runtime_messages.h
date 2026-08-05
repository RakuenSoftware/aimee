#ifndef DEC_AGENT_RUNTIME_MESSAGES_H
#define DEC_AGENT_RUNTIME_MESSAGES_H 1

#include "cJSON.h"
#include <stddef.h>

#define AGENT_FINAL_TOOL_RETRY_LIMIT          2
#define AGENT_DEGENERATE_RESPONSE_RETRY_LIMIT 1
#define AGENT_REQUIRED_EVIDENCE_RETRY_LIMIT   2

int agent_session_retry_final_tool_violation(cJSON *messages, const char *attempted_action,
                                             int *turn, int *max_t, int initial_max_t,
                                             int *retry_count, char *error, size_t error_len);
int agent_session_retry_degenerate_response(cJSON *messages, int *turn, int *retry_count);
int agent_session_retry_required_evidence(cJSON *messages, int *turn, int *max_t, int initial_max_t,
                                          int *retry_count, char *error, size_t error_len);
int agent_required_evidence_keep_tools(int required, int successful_evidence_calls);
/* Whether the evidence gate should still hold the final text turn open.
 * Never on the last usable turn: see the definition. */
int agent_evidence_gate_defers_final_turn(int required, int successful_evidence_calls,
                                          int last_usable_turn);
int agent_required_evidence_reject_response(int required, int successful_evidence_calls,
                                            int is_tool_call, int call_count);
int agent_required_evidence_budget_exhausted(int required, int successful_evidence_calls,
                                             int pre_evidence_responses);
int agent_required_evidence_needs_fallback(int required, int successful_evidence_calls,
                                           int chatgpt_provider, int remaining_calls);
void agent_session_append_final_message(cJSON *messages, const char *content);
void agent_session_append_final_instruction(cJSON *messages);
void agent_session_append_final_retry_instruction(cJSON *messages, const char *attempted_action);
void agent_session_append_degenerate_retry_instruction(cJSON *messages);
void agent_session_append_required_evidence_instruction(cJSON *messages);
void agent_session_append_repository_evidence(cJSON *messages, const char *tool_name,
                                              const char *arguments, const char *result);

#endif /* DEC_AGENT_RUNTIME_MESSAGES_H */
