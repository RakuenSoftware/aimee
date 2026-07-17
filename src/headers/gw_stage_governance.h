/* gw_stage_governance.h -- Slice 2 of the response seam: response GOVERNANCE as a
 * togglable module. Wraps gateway_policy_police_parsed_response in a response stage run
 * through the gw_response_registry, collapsing the 4 inline police call sites
 * (anthropic_http.c messages_buffered/messages_stream x2, openai_chat.c
 * responses_stream_handler) into one path that can be disabled via config. */
#ifndef DEC_GW_STAGE_GOVERNANCE_H
#define DEC_GW_STAGE_GOVERNANCE_H 1

struct parsed_response;

/* The DEPRECATED env default: 1 unless AIMEE_STAGE_GOVERNANCE is an explicit disable token
 * (0/off/false/no). DEFAULT-ON: governance (response tool-policing) must run unless deliberately
 * disabled. The config-store `modules.governance` toggle is now canonical; the wire site resolves
 * it via config_module_enabled() with this as the fallback and passes the result to
 * gw_response_run_governance() as `enabled`. Kept pure so the module stays config-free. */
int gw_response_governance_enabled(void);

/* Run the (togglable) governance response stage over `parsed` via the response registry +
 * runner. `enabled` is the caller-resolved toggle (config-store canonical -> env fallback).
 * Returns the policing intervention (drop) count (>=0), or 0 when governance is disabled or
 * `parsed` is NULL. Behavior-preserving replacement for the inline
 * gateway_policy_police_parsed_response calls. */
int gw_response_run_governance(struct parsed_response *parsed, int enabled);

#endif /* DEC_GW_STAGE_GOVERNANCE_H */
