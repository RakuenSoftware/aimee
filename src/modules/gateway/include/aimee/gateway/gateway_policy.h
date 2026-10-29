/* gateway_policy.h: per-call gateway request/response policy (P2 of the universal
 * gateway). The proxy ingresses (/v1/messages, /v1/chat/completions) run every
 * proxied call through these bounded transforms so aimee can inspect and alter
 * what it forwards — without running the full agent loop. First policy:
 * tool-policing (strip subagent-spawning tools). Reuses guardrails primitives;
 * does not duplicate the agent-loop guardrails. */
#ifndef DEC_GATEWAY_POLICY_H
#define DEC_GATEWAY_POLICY_H 1

#include <aimee/ir/aimee_ir.h>
/* Canonical catalog policing includes namespace children. */
int gateway_policy_apply_ir(aimee_request_t *request);

/* True when the response-side tool-policing policy is enabled. Cheap — one
 * legacy_config_read + one bool read. The streaming /v1/messages path uses this as a
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

#endif /* DEC_GATEWAY_POLICY_H */
