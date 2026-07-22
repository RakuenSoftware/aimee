/* gateway_policy.h: per-call gateway request/response policy (P2 of the universal
 * gateway). The proxy ingresses (/v1/messages, /v1/chat/completions) run every
 * proxied call through these bounded transforms so aimee can inspect and alter
 * what it forwards — without running the full agent loop. First policy:
 * tool-policing (strip subagent-spawning tools). Reuses guardrails primitives;
 * does not duplicate the agent-loop guardrails. */
#ifndef DEC_GATEWAY_POLICY_H
#define DEC_GATEWAY_POLICY_H 1

#include "agent_protocol.h" /* parsed_response_t — type of the police function's argument */

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

/* Model-pin (universal-gateway P2b): when config `gateway_pin_model` is on, force
 * `req`'s "model" to `agent_model` (the configured primary), overriding whatever
 * model the client requested. No-op when the policy is off, `agent_model` is empty,
 * or `req` already names it — so it is byte-neutral by default (the P1 passthrough
 * still honors the client model). Single-model Anthropic-compatible shims enable it
 * so an arbitrary client model name is not forwarded and rejected upstream. Returns
 * 1 if it changed the model, 0 otherwise (for the caller's audit row). */
int gateway_policy_pin_model(struct cJSON *req, const char *agent_model);

/* Response-side tool policing (universal-gateway P2c). Companion to
 * gateway_policy_apply_request; the request side strips a denied tool from the
 * outbound `tools` array, the response side strips a `tool_use` block the served
 * model emitted anyway. Same canonical mapping (`guardrails_canonical_tool_name`
 * == "Subagent") and same gate (`config.gateway_prevent_subagents`); the predicate
 * reads the config so the caller does not need to. */

/* True if `name` would be denied by the active response-side policy. Reads
 * `config.gateway_prevent_subagents` internally — the caller need not gate. The
 * canonical-tool-name mapping is shared with the request side, so a denied name
 * matches the request-side `is_subagent_tool_name` rule (Task / Agent / spawn_agent
 * / RemoteTrigger, etc., all mapped to "Subagent"). */
int gateway_policy_is_denied_tool(const char *name);

/* True when the response-side tool-policing policy is enabled. Cheap — one
 * config_load + one bool read. The streaming /v1/messages path uses this as a
 * single-branch dispatcher: when ON, the upstream is buffered + policed +
 * replayed as SSE (via emit_message_as_sse); when OFF (the default), today's
 * incremental relay/translator runs unchanged. */
int gateway_prevent_subagents_enabled(void);

/* Register a provider reporting whether usable aimee delegates exist (the server
 * owns the agent roster; this CORE module must not read agent state). When it
 * returns nonzero, sub-agent prevention is active regardless of the config flag —
 * enforce-delegate-only for any provider proxied through the gateway. The provider
 * runs on the gateway hot path and is expected to cache. NULL clears it. */
void gateway_policy_set_delegates_available_provider(int (*provider)(void));

/* Mutate `p` in place: memmove-compact denied entries out of `p->calls[]` so the
 * surviving `p->calls[0..p->call_count-1]` is a contiguous prefix of the original
 * (no realloc, no flag-and-skip). `p->call_count` is decremented to match.
 * `p->stop_reason` is finalized as follows:
 *   - All-dropped (call_count == 0) → "end_turn" (the wire's tool_use reply
 *     became an end_turn reply).
 *   - Upstream set a reason (stop_reason[0] != '\0', e.g. "max_tokens",
 *     "stop_sequence", "refusal", "tool_use") AND some calls survive →
 *     preserve verbatim. Partial drops must not lose the upstream's
 *     truncation signal by re-deriving from call_count.
 *   - Upstream didn't set a reason AND some calls survive → "tool_use"
 *     (derive from call_count, since the upstream's omission means
 *     "default tool_use").
 * The renderer's `n_calls > 0 ? "tool_use" : "end_turn"` line 435 of
 * anthropic_ingress.c is replaced (in this PR's diff) with
 * `parsed->stop_reason[0] ? parsed->stop_reason : n_calls > 0 ? "tool_use"
 * : "end_turn"` so the post-police struct's stop_reason is wire-consistent
 * with the wire shape. The `stop_reason` value also feeds the
 * pre-existing `agent_record_token_audit` row in `messages_buffered()` (it
 * reads `parsed.stop_reason`), so the audit log matches the wire.
 *
 * Fields the function does NOT touch (by design): `id`, `name`, `is_tool_call`,
 * `content`, `assistant_message`, `prompt_tokens`, `completion_tokens`,
 * `cache_write_tokens`, `cache_read_tokens`, `model`. None of these are
 * call-indexed. The function DOES write `arguments` on dropped entries —
 * `free(src->arguments); src->arguments = NULL;` — so the dropped-entry
 * strings are not leaked. (Without this, `agent_free_parsed_response`'s
 * 0..call_count-1 sweep would miss them: the survivors overwrite the front
 * slots, but the tail slots still hold the dropped strings, and the sweep
 * never reaches the tail.) Bounded by the original `AGENT_MAX_TOOL_CALLS`
 * ceiling.
 *
 * Returns the number of entries dropped (>= 0; never < 0). 0 = policy off / no
 * denied entries / null `p` — no-op in all three cases. Currently discarded
 * at the call site (messages_buffered ignores the return value); the future
 * P2b audit pass will surface the count alongside the request-side
 * `gateway_policy_apply_request` return value. */
int gateway_policy_police_parsed_response(parsed_response_t *p);

#endif /* DEC_GATEWAY_POLICY_H */
