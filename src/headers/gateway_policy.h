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

/* Strip subagent-spawning tool entries from a bare `tools` array, in place — the
 * array core of gateway_policy_apply_request, for ingresses that carry `tools` as a
 * standalone array rather than inside a request object (e.g. the OpenAI /v1/responses
 * path). Config-gated identically (no-op unless `gateway_prevent_subagents`). Does
 * NOT touch any enclosing tool_choice or drop the array when emptied — the caller
 * owns those (a fully-stripped array should be omitted from the provider request).
 * Returns the number of entries removed (0 = no-op / policy off / not an array). */
int gateway_policy_strip_tools(struct cJSON *tools, int tools_openai_shape);

#endif /* DEC_GATEWAY_POLICY_H */
