/* gw_stage_governance.h -- Slice 2 of the response seam: response GOVERNANCE as a
 * togglable module. Wraps gateway_policy_police_parsed_response in a response stage run
 * through the gw_response_registry, collapsing the 4 inline police call sites
 * (anthropic_http.c messages_buffered/messages_stream x2, openai_chat.c
 * responses_stream_handler) into one path that can be disabled via config. */
#ifndef DEC_GW_STAGE_GOVERNANCE_H
#define DEC_GW_STAGE_GOVERNANCE_H 1

struct parsed_response;

/* 1 unless AIMEE_STAGE_GOVERNANCE is an explicit disable token (0/off/false/no,
 * case-insensitive). DEFAULT-ON: governance (response tool-policing) must run unless
 * deliberately disabled. NOTE: the response/orchestration-stages roundtable ruled the
 * config-STORE the canonical surface for this security-sensitive toggle; the env gate here
 * mirrors AIMEE_STAGE_MEMORY and is superseded by the dedicated config-surface slice. */
int gw_response_governance_enabled(void);

/* Run the (togglable) governance response stage over `parsed` via the response registry +
 * runner. Returns the policing intervention (drop) count (>=0), or 0 when governance is
 * disabled or `parsed` is NULL. Behavior-preserving replacement for the inline
 * gateway_policy_police_parsed_response calls. */
int gw_response_run_governance(struct parsed_response *parsed);

#endif /* DEC_GW_STAGE_GOVERNANCE_H */
