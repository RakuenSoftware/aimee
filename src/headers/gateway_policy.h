/* gateway_policy.h: per-call gateway request/response policy (P2 of the universal
 * gateway). The proxy ingresses (/v1/messages, /v1/chat/completions) run every
 * proxied call through these bounded transforms so aimee can inspect and alter
 * what it forwards — without running the full agent loop. First policy:
 * tool-policing (strip subagent-spawning tools). Reuses guardrails primitives;
 * does not duplicate the agent-loop guardrails. */
#ifndef DEC_GATEWAY_POLICY_H
#define DEC_GATEWAY_POLICY_H 1

struct cJSON;

/* Apply request-side tool policing to a proxied request, in place. `tools` is read
 * from `req`; entries are matched by tool name regardless of API shape
 * (`tools_openai_shape`: 1 = OpenAI [{type:function,function:{name}}], 0 = Anthropic
 * [{name}]). When config `gateway_prevent_subagents` is on, subagent-spawning tools
 * (via guardrails_is_subagent_tool) are removed, an empty `tools` array is dropped,
 * and a `tool_choice` that names a removed tool is relaxed to auto. Returns the
 * number of tools stripped (0 = no-op / policy off), for the caller's audit row. */
int gateway_policy_apply_request(struct cJSON *req, int tools_openai_shape);

#endif /* DEC_GATEWAY_POLICY_H */
