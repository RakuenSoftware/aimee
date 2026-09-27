# Buffered provider attempt receipts — 2026-09-23

Versioned memory source handles now bind each participating buffered provider
attempt to canonical Go metadata: distinct attempt and producer IDs, request and
turn identity, authenticated request binding, scope, observed source versions,
source-check challenge, provider/model/route, producer build, caller and operator
limit commitments, exact final body SHA-256 and decimal byte count. Token counts
remain null. Metadata explicitly covers retained versioned inputs and uses
`commitment_only` retention: it stores no prompt or credential headers and cannot
reconstruct an erased input or unavailable final body.

The Go response is a plan with `durable:false`, never a durability promise.
Before each actual attempt, including after retry backoff, the host revalidates
sources and synchronously appends both `prepared` and `dispatch_admitted` to the
existing native WORM ledger. It uses that owner's idempotent fsync-durable append,
not the asynchronous observability queue. An unavailable append refuses dispatch.
The ordinary source-check API remains compatible; one successful check can bind
only one receipt. Attempts and observations are request-bound, bounded and
expired independently of durable WORM retention. Oversized metadata fails rather
than being truncated into the audit owner's detail limit.

The HTTP loop carries explicit per-call callbacks and stack-local attempt state;
nested owner HTTP calls cannot inherit them. After a transport returns, an HTTP
response records `acknowledged` with status and a commitment to the host's buffered
response string. This is not semantic success or confirmation of an external
effect. A network failure or timeout records `outcome_unknown`. Missing or failed
observation persistence leaves durable admission unresolved: it never proves
that bytes were not sent. Successful provider responses are not resent to repair
an audit failure. Idempotent observation retries preserve the same timestamp and
reject conflicting evidence.

Current integration covers native tool/non-tool model calls and model fallbacks,
buffered Responses, buffered Anthropic, and Anthropic's buffered replay path.
The latter two retain one transport attempt; this change does not add retries.
Requests without a versioned source handle do not acquire receipt coverage.
The follow-up described below adds incremental stream receipts. Observed transport
start, recovery inspection/ownership resolution, authorized receipt lookup and
verification, unversioned channels, full policy/index metadata and external
checkpoint comparison remain open. This is not MR-06 completion.

[Go contract tests](memory-provider-receipts-2026-09-23/contracts-race.txt) cover
single-use admission, distinct attempts, lossless large counts, changed payload,
route, model, build and limits, foreign principals, malformed/oversized metadata,
capacity bounds, expiry, idempotent acknowledgements and transport uncertainty.
[Socket tests](memory-provider-receipts-2026-09-23/http.txt) prove a missing initial
admission causes zero attempts and zero response observations, while accepted
retries preserve exact bytes and receive separate before/after callbacks.
[Native ingress tests](memory-provider-receipts-2026-09-23/ingress.txt) use the real
Go owner and WORM store, verify the body commitment (including embedded NUL),
reopen durable admission without inventing acknowledgement, and inject failure
of the admission append after successful preparation. This is a store-reopen
check, not a process-crash experiment. [Agent tests](memory-provider-receipts-2026-09-23/agent.txt)
preserve refusal through both execution paths and their fallbacks.

The [final full PostgreSQL race suite](memory-provider-receipts-2026-09-23/full-race.txt)
and [exported owner build](memory-provider-receipts-2026-09-23/export.txt) pass.
Native retry, ingress, agent and buffered provider adapter builds/tests pass;
C ownership, bus-boundary and descriptor guards pass. The buffered candidate
`c3919b1a8` passed fresh T2/T3 deployment checks: 1,630/1,630, both processes
exit 0. [Raw receipts and nine container identities](memory-provider-receipts-2026-09-23/fresh/image-identities.json)
bind application image `sha256:2ef35cb0dd6ec8f4954a302766080ea9056e35ff1028f5e447e74a269d5fc3b7`
and the schema-34 database image. This existing matrix checks adapter behavior;
it does not directly inspect durable receipt rows.
Released CT100 remains 0.4.5; the draft changes do not replace its application.


## Incremental stream follow-up

Incremental native Anthropic, OpenAI IR relay and legacy OpenAI relay attempts
now use the same synchronous prepared/admitted persistence gate. The transport
hashes exact provider chunks before forwarding them, with constant-size SHA-256
state and a decimal byte count. Observations distinguish `provider_stream_bytes`
from `host_buffered_response_string`; older hosts that omit the representation
retain the buffered interpretation. Explicit malformed representations fail.
A downstream callback abort records the observed prefix with an unresolved
transport outcome. Observation persistence failure never resends a provider
request. Admission failure emits one terminal error and no synthetic successful
stream ending.

[Transport tests](memory-provider-receipts-2026-09-23/stream-wire-fence.txt)
cover all three routes, zero network calls on refusal, exact embedded-NUL and
Unicode chunk commitments, partial callback failure, observation failure and
requests without versioned source handles. [Actual handler tests](memory-provider-receipts-2026-09-23/stream-anthropic-http.txt)
exercise buffered and streaming refusal across the three provider drivers and
both proof modes, plus the IR relay helper. Both minimal adapter fixtures pass.
[Go representation contracts](memory-provider-receipts-2026-09-23/stream-contracts-race.txt)
pass under the race detector; the [exported owner build](memory-provider-receipts-2026-09-23/stream-export.txt)
also passes. These are native/owner tests; fresh deployment results for the
preceding buffered candidate do not certify this later streaming change.


The streaming candidate `5b424e1bf` passed fresh T2/T3 suites with 1,630/1,630
checks and both processes exiting 0. [Nine image identities](memory-provider-receipts-2026-09-23/stream-fresh/image-identities.json)
bind application `sha256:ec72ea38d252c79fd165e1135e8af2f35d1c8626184e2f3ef14f5ab017e2c861`.
Those adapter checks still precede the direct ledger-at-arrival assertions added
with supplied receipt verification. Their success does not imply coverage of
unversioned native recall; the next experiment exposed that specific gap.
