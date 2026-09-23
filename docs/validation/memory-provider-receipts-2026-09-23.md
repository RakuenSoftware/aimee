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
Incremental streaming still uses its existing source fence. Observed transport
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
C ownership, bus-boundary and descriptor guards pass. Fresh deployment evidence
for this receipt change is pending.
Released CT100 remains 0.4.5; the draft changes do not replace its application.
