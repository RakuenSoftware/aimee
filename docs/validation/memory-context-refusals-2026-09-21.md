# Memory context refusal propagation — 2026-09-21

## Change and scope

Native agent context assembly previously discarded non-success Go recall envelopes.
Its initial caller substituted the original prompt when assembly returned NULL;
refresh retained the old context. A required-context refusal could therefore be
followed by a provider request without the required context.

The checked host assembly API now preserves the owner's status/kind/message and
returns no context on an explicit non-success recall envelope. Initial native
execution stops before authentication/provider dispatch. Refresh stops before the
next request and runs the normal resource cleanup. The distinct
`AGENT_RC_CONTEXT_REFUSED` result is terminal in generic routing, configured
fallback chains and same-tier fallback loops; it does not charge provider health
with a memory failure. Preview/dry-run consumers report the same diagnostic.
Refresh retains the configured role when rebuilding context.

MCP preserves every explicit non-success recall envelope, including quarantine
and degraded states, before it can detach an empty recall or add session guidance.
The exact S1 source-review record and external memory ownership ledger were
reviewed with these host/fixture changes. Memory decisions remain in Go. No C bus
implementation changed.

## Local evidence

- `unit-test-agent-context-refusal` links the production builder and native
  executor. Module and HTTP transports supply controlled external responses.
  Overflow, quarantine and unavailable initial responses each produce zero
  provider calls. An initial success followed by refusal at turn-five refresh
  stops after exactly five provider calls; no sixth request uses stale context.
  A subsequent invocation on the same thread succeeds with one provider call.
  No live provider is called by this fixture.
- `unit-test-mcp-directive-transport` invokes the actual MCP consumer and verifies
  byte-identical error/quarantine/degraded envelopes for private/shared stores
  and both session-start settings (12 combinations).
- `unit-test-agent-error-retryable` verifies the terminal result even when the
  owner diagnostic contains a retryable-looking HTTP status or quota message.
- Native Server build and all 77 lint checks pass. All 767 benchmark tests pass with two existing
  skips, including the exact S1 integration-review checks.

Fresh deployment and remote CI results for this change must be recorded against
the pushed revision before treating this as deployment acceptance.

## Remaining acceptance

This does not complete MR-03. HTTP ingress still has an omission path that loses
assembly failures. CLI subprocess hosts, absent/malformed transport replies,
complete native hard-rule rendering, protected caller constraints, provider token
counting, source-version binding and durable release/dispatch receipts remain
unproven or unfinished. Existing optional unavailability behavior is not converted
into a new policy by this change. No latency improvement is claimed.
