# P2b-a — buffered kb Bedrock egress authority

- **State:** proposed implementation slice; requires roundtable convergence before code.
- **Depends on:** P1 identity/RLS, P2a catalog, P3a pricing/audit, P4a budget,
  P4b rate, P6a/P6b/P6c Bedrock cores, and P7 signed-HWM use-in-place.
- **Scope boundary:** one real, certificate-authenticated, non-streaming Bedrock Converse
  path. This is deliberately not the server roster/proxy, OIDC propagation, STS,
  streaming, or a generic vendor abstraction.

## Outcome

An enrolled mTLS client may call `POST /v1/llm/egress` with a strict OpenAI-chat
payload and a catalog model ID. aimee-kb derives one billing team, catalog target,
price version, and vault credential binding from PostgreSQL; admits rate, maximum
budget, and content-free token audit before key use; asks the attested vault primitive
to create an owned signed Bedrock request; cleanses the key before network I/O; sends
the request through the P6 transport; and returns an OpenAI-chat response rendered
from canonical IR. The key, provider target, credential slot, price, and endpoint are
never caller-controlled and never leave kb.

## Fixed first-slice protocol

The only public shape is:

```json
{
  "request_id": "caller-stable-id",
  "team_id": 42,
  "project_id": 7,
  "model_id": "org/bedrock-model",
  "stream": false,
  "payload": {"model":"org/bedrock-model","messages":[],"max_tokens":256,"stream":false}
}
```

`team_id` and `project_id` are optional; absent team means the certificate principal's
default team, and absent project means team-wide attribution. `request_id`,
`model_id`, `stream:false`, and `payload` are required. Reject unknown or duplicate
envelope keys, query strings, trailing JSON, non-integral IDs, embedded NUL/control
characters, invalid UTF-8, and values outside explicit caps. `payload` is parsed with
the existing OpenAI Chat frontend into `aimee_request_t`; both model fields must match,
both stream fields must be false, and `max_tokens` must be an explicitly present,
positive integer. Provider, wire, region, endpoint, headers, credential identity,
pricing version, token ceilings, and retry controls do not exist in the request shape.

The response is `openai_frontend_render(aimee_response_t)`, augmented only with the
stable `request_id` and the resolved numeric `team_id`. Errors are content-free JSON
with stable classes. No prompt, response, provider body, authorization header, or
credential is logged or persisted.

## Transport identity and HTTP boundary

1. Extend `kb_reqctx` with a distinct verified transport principal, copied and
   cleared with the actor. `kb_tls_serve_conn` obtains issuer and serial from the
   verified peer certificate and constructs the canonical principal with the existing
   identity helper. The authority/idempotency origin is the canonical
   `cert:<issuer>:<serial>` identity key. CN remains a display/scope label only and
   cannot select team, key, model, or idempotency ownership. Keep actor storage intact
   for P5, but P2b-a rejects a missing certificate transport principal and resolves
   with `actor=NULL`.
2. Harden the mTLS HTTP reader before exposing egress: parse exactly one
   case-insensitive `Content-Length`, reject duplicates/conflicts, signs, junk,
   transfer-encoding, overlong header lines, and lengths above a dedicated bounded
   egress cap; distinguish incomplete header/body from a complete request; never pass
   bytes beyond the declared body. Apply socket/SSL read and write deadlines.
3. Replace the listener's single blocking connection loop with a fixed-size bounded
   worker pool and bounded accepted-fd queue. Saturation closes/refuses without
   allocating request-sized buffers. Each worker owns one connection, always clears
   `kb_reqctx`, and has no cross-request TLS state. Shutdown closes the listener,
   drains/cancels queued fds, joins workers, then frees the shared `SSL_CTX`.
4. Set the transport principal before routing and clear it on every exit. Unit tests
   prove that sequential requests on a reused worker cannot inherit identity.

## Authoritative private binding

Add a private `org_egress_binding` table keyed by `(team_id, model_id)`. It contains
only authority metadata: `billable_model`, immutable/current `pricing_version`,
`key_id`, the vault `(principal,agent,cred)` slot, positive `max_input_tokens`, positive
`max_output_tokens`, enabled state, and timestamps. It contains no plaintext or
ciphertext. Runtime gets no direct table access and the public entitled-model roster is
unchanged.

Add audited, admin-only SECURITY DEFINER bind/disable functions for operator setup and
rotation, with exact foreign-key/team/model checks. Binding changes append a
secret-free WORM record in the same transaction. The binding function pins the current
immutable pricing version and rejects an unpriced model, non-Bedrock/Converse catalog
row, missing vault slot binding, or invalid ceiling. No public HTTP mutation route is
added in this slice; expose the operation through a narrow existing kb operator CLI
command and typed DB2 wrapper so CT260 and operators never need raw table DML.

Add a single SECURITY DEFINER runtime resolver returning one opaque typed structure for
the exact `(current principal, current team, requested model)`. In one primary
transaction it rechecks team membership, exact entitlement, catalog enabled/provider/
wire/Bedrock adapter constraints, binding enabled, pinned immutable pricing existence,
and active credential-slot identity. Every unavailable condition collapses to one
denial result—no catalog/key/pricing existence oracle. Its private fields are never
serialized to the client or logs. The existing P6 authorized-target resolver remains
the only constructor of the opaque network target, and must resolve within this same
tenant transaction so the binding and catalog snapshot cannot interleave with a
mutation.

## Canonical request and maximum reservation

After strict parsing, discard authority-bearing raw sidecars and validate the typed IR
against explicit P6 bounds. Canonicalize a domain-separated, length-prefixed digest
over: protocol version, canonical transport identity, request ID, resolved team,
optional project, catalog model, normalized typed IR, and generation parameters. Do
not hash raw JSON bytes. Equivalent accepted JSON encodings must produce one digest;
semantically different requests must not collide. This 64-lowercase-hex digest is the
P7 key-use digest.

For this conservative first slice, reserve the binding's entire `max_input_tokens`
plus the request's explicit `max_tokens` (which must not exceed
`max_output_tokens`). This knowingly over-reserves but cannot understate provider
input usage. A new definer/wrapper computes a NUMERIC maximum using only the pinned
immutable price row and returns the price version plus decimal amount; unpriced,
overflowing, negative, or out-of-range results fail closed. Settlement uses actual
non-negative IR usage counts and the same pinned version; usage above either admitted
ceiling is an integrity event and settles the reservation at its full maximum.

## Durable admission and dispatch state machine

Add private `org_egress_dispatch`, unique on `(origin_identity,request_id)`, with
immutable request digest, team/project/model/binding/price/reserved-maximum fields and
states:

`admitted -> dispatching -> succeeded | denied | failed | uncertain`

Terminal rows are immutable except that a separate future operator reconciliation may
move `uncertain` to a proven terminal state; P2b-a exposes no redispatch transition.
Store only content-free outcome class, authenticated HTTP status when known, usage,
audit/reservation identifiers, and timestamps—never request/response bodies or signed
headers. WORM guards reject immutable-field changes, illegal transitions, delete, and
truncate. Runtime has only SECURITY DEFINER operations.

One short PostgreSQL transaction performs:

1. certificate-only identity resolution and exact team/project validation;
2. private binding/catalog/pricing resolution;
3. idempotency claim on `(canonical origin,request_id,digest)`;
4. `org_rate_check`;
5. budget reservation at the conservative maximum;
6. `org_token_audit_start`; and
7. dispatch-row insertion in `admitted`.

Any denial/error rolls the whole transaction back, including the rate-window bump.
An exact existing request returns its durable state and never re-admits, reuses a key,
or dispatches; a digest/binding mismatch is an integrity conflict. Concurrent twins
have one owner. The route changes `admitted -> dispatching` durably before calling the
vault. From that point no automatic code path may retry credential use or vendor
dispatch.

If key admission/signing is definitively refused before an intent can have committed,
settle `failed` at zero. If key-use returns replay, integrity, or any result that can
mean the intent committed without producing an owned request, mark `uncertain` and
retain/charge the full reservation. This avoids inventing a safe retry across the P7
commit/callback crash gap. Crash recovery sweeps stale `admitted` or `dispatching`
rows to `uncertain`, pairs them with `org_budget_settle_expired`, and moves the token
audit to `indeterminate`; it never touches a key or network.

For an authenticated complete vendor response, settle dispatch, budget, and token
audit in one transaction. Success records actual usage/cost. A proven provider denial
with no usage settles at zero only when the transport proves the complete response was
authenticated and the provider class is explicitly non-billable; all other provider
errors conservatively charge the maximum. Any impossible/overflowing usage charges
the maximum and records integrity failure.

## Vault-sign/network split

Do not call `kb_bedrock_dispatch_buffered` from the plaintext callback. Split the P6
surface into two ownership-safe operations:

- an internal builder accepts the opaque authorized target, typed IR, and borrowed
  AWS credential view; it creates one initialized, owned `kb_bedrock_wire_request_t`
  containing the body, target-derived host/path, payload hash, access-key identifier,
  session token if any, and signature—but never the secret access key;
- a dispatcher accepts only the opaque target plus that owned signed request and does
  TLS/network/response decoding without any raw credential parameter.

The P7 callback strictly parses one bounded vault plaintext JSON object containing
`access_key_id`, `secret_access_key`, and optional `session_token`, rejects duplicate
or unknown keys, points credential views into the locked arena, builds the signed
owned request, and returns. The existing P7 cleanup completes before any DNS, socket,
TLS, or write. Clear the signed request on every exit, including authorization and
transport failures. Deprecate the combined raw-credential dispatch entry point from
production kb callers; pure P6 tests may retain an internal test wrapper.

Extend the transport result with a conservative `vendor_bytes_possible`/authenticated
completion classification. A complete success can settle actual usage. Once vendor
bytes may have been written, timeout, EOF, cancellation, framing error, or local crash
is `uncertain` and keeps the maximum charge. Definite pre-write failures are `failed`
and may settle zero, but never redispatch under the same request ID.

## WORM witness release gate

P7's local WORM intent is necessary but the off-host witness/outbox is not yet landed.
Add one explicit egress readiness predicate/configuration seam: production org-key
egress defaults disabled unless the off-host WORM witness reports ready. The CT260
test profile may enable a conspicuously named test-only override only with a real
custody/HWM provider and must emit a startup warning plus a WORM event. File/mock
custody remains forbidden. The override is unavailable in hardened/production builds.
When the remaining P7 witness slice lands it replaces the predicate implementation,
not the route contract.

## HTTP status contract

- `200`: proven complete provider success.
- `400`: malformed/unsupported envelope or payload.
- `401`: absent/revoked certificate.
- `403`: unresolved team/project, entitlement, private binding, or sealed/not-ready
  egress (collapsed authority denial).
- `409`: request-ID mismatch, exact replay/in-flight/terminal replay, or uncertain
  outcome; body contains only request ID and durable state.
- `429`: rate or budget refusal, without revealing which private policy bound it.
- `502`: proven provider denial or definite pre-write failure.
- `503`: local admission/custody/database failure before any possible vendor write.
- `504`: ambiguous/post-write timeout, persisted as `uncertain` before response.

## Code units

1. **P2b-a1 — schema and DB2:** private binding, opaque resolver, conservative pricing,
   atomic admission, dispatch machine/sweeper, grants, SQLite shape-only mirrors, and
   real-PG concurrency/rollback/WORM tests.
2. **P2b-a2 — transport identity and HTTP safety:** reqctx transport principal,
   strict framed reader/deadlines, bounded listener workers, route registration, and
   identity/leak/saturation tests.
3. **P2b-a3 — IR/admission/sign/dispatch orchestration:** strict parser/digest,
   vault credential parser and signed-request ownership split, durable outcome
   handling, response render, failpoints, and focused sanitizers/fuzzers.
4. **P2b-a4 — live gate:** operator binding command, CT260 fixture/scripts, real
   PG17+mTLS+signed HWM/mock Bedrock proof, documentation/status tally.

Each unit is independently committed and verified; the branch is adversarially
roundtable-reviewed after the live gate and every valid finding is fixed before merge.

## Validation gates

### Local/unit

- Strict-envelope corpus/fuzzer: duplicate/unknown keys, numeric edge cases, UTF-8,
  depth/node/body caps, mismatched model/stream, missing/unbounded max tokens, and
  canonical digest equivalence/difference.
- Identity: issuer+serial—not CN—owns team and idempotency; same CN/different serial is
  distinct; same serial/different issuer is distinct; transport/actor confusion and
  worker-thread residue deny.
- Atomic admission: rate increment rolls back on budget/audit/dispatch failure; budget
  rolls back on later failure; exact replay never increments, reserves, audits, signs,
  or sends; conflicting replay denies; concurrent twins produce one owner.
- Pricing: unpriced/pointer change/overflow/zero ceiling deny; maximum reservation is
  conservative; actual settlement uses pinned version; over-ceiling usage charges max.
- Vault split: plaintext parser is strict; callback performs no network; secret key is
  absent from owned request after callback; arenas and all copies cleanse under
  ASAN/UBSAN/leak checks; seal and HWM failures never send.
- State/failpoints at every boundary: after admission, after dispatching, after P7
  intent, after signed-request build, before first byte, after partial write, after
  authenticated headers/body, and before settlement. No failpoint permits a second
  dispatch. Sweeper makes stale work uncertain/content-free.
- HTTP workers: slowloris/read timeout, malformed Content-Length, saturation, shutdown,
  SIGPIPE, cancellation, and response-size bounds.
- Static scans assert no org key/server vault path, no credential/authorization/body in
  logs/DB fixtures, and no route-accessible raw target/credential seam.

### Real targets

1. Regenerate schema and build `server`, `kb`, lint, and focused unit/fuzz targets.
2. CT103 real PG17: in-place schema upgrade, grants/RLS/FORCE posture, definer
   search-path attacks, WORM failure atomicity, identity/team inverse cases, admission
   concurrency, lease recovery, then the full P1 RLS gate.
3. CT260: use its real PG17, swtpm/signed-HWM-capable custody configuration, production
   kb mTLS listener, independently enrolled client certificate, and the independent
   P6 mock Bedrock TLS endpoint. Provision a team/member/default/project, entitled
   catalog model, price, rate, budget, private binding, and attested vault credential
   only through supported operations.
4. Prove one entitled request returns a completion and exactly one matching rate
   window, reservation settlement, token audit/rollup, dispatch terminal row, P7
   intent, and WORM event. Scan process environment, files, logs, DB, response, and
   server-side fixtures for the secret canary and prompt canary.
5. Prove unentitled, wrong-team/project, disabled/unpriced binding, revoked cert,
   sealed/HWM rollback, budget/rate refusal, bad mock TLS, provider error, partial
   response, timeout, and crash failpoints produce the specified durable outcomes and
   zero unintended sends. Retry every request ID and show the mock observed at most
   one dispatch.

## Explicitly deferred

- P2b-b true streaming, reservation heartbeats during long streams, and streaming
  response backpressure.
- P5 OIDC actor propagation and the server→kb blended-roster/proxy topology.
- Generic Anthropic/OpenAI/Responses vendors, STS/role assumption and credential
  caches, regional failover, and automatic retry.
- Off-host WORM outbox/witness implementation, operator reconciliation of uncertain
  requests, scheduled expiry service, and compromise fencing (remaining P7).
- P8 thin-client certificate presentation/runtime integration.

