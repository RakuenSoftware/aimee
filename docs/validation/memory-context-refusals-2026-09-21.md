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
with a memory failure. Credential-pool retry and final lease release also preserve
the refusal without rotating or penalizing a credential. Preview/dry-run consumers
report the same diagnostic.
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
- `unit-test-delegate-credentials` drives the production retry wrapper and lease
  pool with controlled executor outcomes. Initial refusal and refusal after a
  genuine provider rate limit stop without another rotation or penalty, with
  tools enabled and disabled (four combinations).
- Native Server build and all 77 lint checks pass. All 767 benchmark tests pass with two existing
  skips, including the exact S1 integration-review checks.

## Fresh deployment evidence

Implementation/harness `2123e9a592c9035a6f214f426dc7ed643fb5653a` was built and
validated in fresh task-owned Docker projects on `.253`, CT 9498:

- T2: **778/778** checks ([receipt](memory-shared-reliability-2026-09-21/fresh-t2-2123e9a592.json)).
- T3: **483/483** checks ([receipt](memory-shared-reliability-2026-09-21/fresh-t3-2123e9a592.json)).
- Total: **1,261/1,261**, including 273 provider-boundary checks per placement.
- Actual application image: `sha256:e3f4738f0cae5ed1f9000eb4577e0aa5fe1907a19f746d28c383a72b14583813`.
  [Image identities and verified deployment limits](memory-shared-reliability-2026-09-21/image-identities-2123e9a592.json)
  preserve the app, PostgreSQL and embedder identities without credentials.
- [Provider byte accounting](memory-shared-reliability-2026-09-21/provider-accounting-2123e9a592.json)
  contains byte counts/digests, not prompt bodies or provider headers.
- All nine task-owned containers are stopped; volumes and receipts are retained.

The native initial/refresh refusal regressions above use controlled module/HTTP
transport responses; the deployment matrix is broader integration regression
coverage, not a live native-agent refusal test. Follow-up `b3a2578ae3` adds the
credential retry/release guards and four retry cases. Its targeted credential
suite, native build and all 77 lint checks pass locally; the fresh image above
predates that follow-up. All 58 remote CI checks pass on the subsequent evidence revision `1139676c20`.


## HTTP request refusal follow-up

The host now retains the first required Go gateway-plan or ingress assembly
failure in its existing request context. The final provider fence checks it before
optional reduction or byte admission. Successful empty/inactive plans remain valid;
optional retrieval-outcome reporting does not refuse a request. A later successful
assembly cannot erase an earlier refusal. HTTP lifecycle clear/set removes the
outcome, and asynchronous copies carry it into their worker. Native execution
preserves the terminal result when it inherits this HTTP outcome.

The generic IR plan executor exposes an optional host failure callback, so a plan
transport failure or malformed plan cannot bypass the ingress failure sink by
preventing ingress from running at all. This adds no memory decision to the IR
module. The HTTP status is obtained from the existing Go runtime-web provider;
its absence retains the transport's 502 fallback. This path preserves the owner
kind, not its full message/envelope.

Local evidence includes real Go ingress assembly and final-fence tests for
transport failure, explicit refusal, malformed success replies and invalid
allocation; request-copy/clear/thread-isolation checks; generic plan failures and
valid empty plans; native inherited refusal with zero provider calls; and actual
buffered/streaming Anthropic-compatible handlers targeting all three provider
formats with reduction enabled/disabled and zero provider calls. The full native
build, all 77 lint checks and 767 benchmark tests (two existing skips) pass. The fresh
`2123e9a592` deployment receipt above predates this follow-up; a new deployment
run is required. No additional whole-request latency claim is made.

## Fresh follow-up findings

The first fresh `fc0108f6a2` run exposed a previously hidden optional-source
status mismatch: the KB transport reports `status:unavailable` when no shared
KB is configured, while Go ingress recognized only `status:error`. The new
required-assembly fence correctly surfaced the resulting `invalid_projection`;
previous serving silently dropped the entire envelope. Go now recognizes the
transport's known non-success statuses as unavailable optional evidence, while
malformed responses and invalid successful projection commitments still refuse.
Real Go and C-host/Go-process regressions cover both sides; the full Go memory
suite passes with required PostgreSQL replay/evaluation enabled.

The live outage harness also initially expected memory-disabled native Anthropic
passthrough to require memory. Its corrected oracle checks that this path retains
provider dispatch while the ten memory-dependent frontend/provider/streaming
combinations refuse with zero provider requests. Owner restart readiness now
runs in cleanup even after an assertion fails. The initial fresh runs are failed
receipts, not release evidence; corrected fresh validation is still required.

## Briefing allocation follow-up

The Go briefing owner's promoted `evidence_heavy` style previously raised every
explicit allocation below 3,000 estimated tokens to 3,000. It now uses that value
only for an unspecified allocation; explicit allocations in the existing legacy
64–8,192 range remain upper bounds. Candidate limits and priority stay unchanged.
The required PostgreSQL replay exercises promoted style at 64, 128, 1,024, 2,500,
3,000 and 8,192, verifies the returned allocation and complete serialized size,
and preserves the existing default and maximum tests. The full Go memory suite
passes with required evaluation/replay enabled. These are legacy bytes/4
estimates, not provider token counts. The fresh `9deb1efc14` deployment run
predates this separate follow-up.

## Remaining acceptance

This does not complete MR-03. CLI subprocess hosts, native absent/malformed transport replies,
complete native hard-rule rendering, protected caller constraints, provider token
counting, source-version binding and durable release/dispatch receipts remain
unproven or unfinished. Existing optional unavailability behavior is not converted
into a new policy by this change. No latency improvement is claimed.
