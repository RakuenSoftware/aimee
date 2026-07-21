# P2b-a — buffered kb Bedrock egress authority

- **State:** roundtable-converged plan v3; verified minority findings incorporated.
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
This slice proves the complete live path in the integration environment but remains
production-release-disabled until the real off-host WORM witness slice lands; it does
not claim a bypass-backed production gate.

## Fixed first-slice protocol

The only public shape is:

```json
{
  "request_id": "018f6f4e-7d3a-7b1c-9f0a-123456789abc",
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
`request_id` is a canonical lowercase UUIDv4/v7 with 122+ bits of caller-generated
entropy; other shapes are rejected so a team peer cannot cheaply squat predictable IDs.
P2b-a accepts text-only system/user/assistant messages plus ordinary sampling/stop
parameters. Reject images, documents, tool definitions/calls/results, cache controls,
thinking, metadata, service tiers, unknown blocks, and raw sidecar-only extensions;
those features need a later pricing/billing review before admission.

The response is `openai_frontend_render(aimee_response_t)`, augmented only with the
stable `request_id` and the resolved numeric `team_id`. Errors are content-free JSON
with stable classes. No prompt, response, provider body, authorization header, or
credential is logged or persisted.

## Transport identity and HTTP boundary

1. Extend `kb_reqctx` with a distinct verified certificate-instance principal plus
   the verified leaf SHA-256 fingerprint, copied and cleared with the actor.
   `kb_tls_serve_conn` obtains issuer and serial from the
   verified peer certificate and constructs the canonical principal with the existing
   identity helper. The certificate authority, audit, and P7 key-use origin is the canonical
   `cert:<issuer>:<serial>` identity key. CN remains a display/scope label only and
   cannot select team, key, model, or idempotency ownership. Keep actor storage intact
   for P5, but P2b-a rejects a missing certificate transport principal and resolves
   with `actor=NULL`. The listener's cached revocation check remains a fast prefilter
   only. The atomic admission definer must require an existing active enrollment for
   the exact fingerprint and matching normalized issuer/serial, and recheck
   `state/revoked_at` on the primary in the same transaction as every other authority
   decision. Unknown, mismatched, retired, revoked, or DB-unavailable certificates
   fail closed. Fix mTLS renewal in this slice: the new leaf is inserted with its new
   fingerprint and normalized issuer/serial under the same enrollment scope before it
   is returned, while the old leaf remains independently revocable. A renewal DB
   failure returns no usable certificate.
2. Harden the mTLS HTTP reader before exposing egress: reject any Transfer-Encoding
   and malformed or duplicate `Content-Length` globally; permit absent length only
   for a method/route that accepts no body and reject any body octets in that case.
   Require exactly one canonical nonnegative `Content-Length` for egress and every
   other body-bearing route. Reject signs, junk, overlong header lines, and lengths
   above a dedicated bounded egress cap; distinguish incomplete header/body from a
   complete request; never pass bytes beyond the declared body. Apply socket/SSL read
   and write deadlines. Reject bare LF, obsolete folded headers, whitespace before a
   colon, invalid header-name/value bytes, multiple request lines, absolute-form
   egress targets, and bytes after the declared request. Preserve bodyless GET behavior
   for enrollment/metrics routes.
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

After strict parsing, discard authority-bearing raw sidecars and validate the text-only
typed IR against explicit P6 bounds. Before admission, use a new pure P6 serializer to
produce the exact canonical unsigned Bedrock request body that will later be signed.
Hash a domain-separated length-prefixed transcript with literal domain
`aimee.p2b.egress.v1` and fields in this fixed order: canonical transport identity,
request UUID, resolved team, project-presence+value, catalog model, and the exact
canonical Bedrock body bytes. The body includes every accepted message, sampling,
stop, model, and generation field; no second ad-hoc IR field list can drift. Do not
hash raw client JSON bytes. Equivalent accepted JSON encodings produce one digest;
semantically different requests do not. This 64-lowercase-hex digest is the P7 key-use
digest and the same body object is retained for the later signed-request builder.

For this conservative first slice, reserve the binding's entire provider-documented
`max_input_tokens` plus its entire `max_output_tokens`, not merely the requested
completion limit. The request's explicit `max_tokens` must not exceed that output
limit. Accepted IR features and the serialized P6 body remain bounded, and binding
ceilings are operator-validated against the provider model limits. A new
definer/wrapper computes a NUMERIC maximum using only the pinned immutable price row,
charging the full input ceiling at the greatest applicable input/cache-read/cache-write
rate and the full output ceiling at its output rate, and returns the price version plus
decimal amount; unpriced,
overflowing, negative, or out-of-range results fail closed. Settlement uses actual
non-negative IR usage counts and the same pinned version.

Provider-reported usage above either admitted ceiling must not be hidden by P4a's
`LEAST(realized,reserved_max)`. Add a dedicated atomic overage settlement path that
records the full observed liability in audit/rollup and an explicit overage amount,
settles/releases the reservation, and fences that team/model from further egress until
operator remediation. The fence is checked by the atomic admission resolver. This is
an integrity incident: the hard pre-dispatch cap prevented all spend the gateway could
legitimately predict, while accounting still reflects the provider's larger real
liability instead of silently truncating it.

## Durable admission and dispatch state machine

Add private `org_egress_dispatch`, unique on `(team_id,request_id)`, with
immutable request digest, team/project/model/binding/price/reserved-maximum fields and
states:

`admitted -> dispatching | failed`; `dispatching -> succeeded | denied | failed | uncertain`

Terminal rows are immutable except that a separate future operator reconciliation may
move `uncertain` to a proven terminal state; P2b-a exposes no redispatch transition.
Store only content-free outcome class, authenticated HTTP status when known, usage,
audit/reservation identifiers, and timestamps—never request/response bodies or signed
headers. WORM guards reject immutable-field changes, illegal transitions, delete, and
truncate. Runtime has only SECURITY DEFINER operations.

One short PostgreSQL transaction performs:

1. exact active-enrollment/revocation recheck, certificate-only identity resolution,
   and exact team/project validation;
2. lookup/claim the dispatch row before consulting current binding/entitlement/pricing,
   so an authorized exact replay returns its durable state after configuration changes;
   use `INSERT ... ON CONFLICT` plus a row lock on `(team,request_id)`;
3. for a genuinely new claim only, private binding/catalog/pricing resolution;
4. `org_rate_check`;
5. budget reservation at the conservative maximum;
6. `org_token_audit_start`, then commit the already-inserted `admitted` claim.

Any denial/error rolls the whole transaction back, including the rate-window bump.
An exact existing request returns its durable state and never re-admits, reuses a key,
or dispatches; a digest/origin/binding mismatch is an integrity conflict. The team-
scoped idempotency namespace deliberately survives certificate renewal while the
billing team is unchanged, so retrying an old request after renewal cannot acquire a new dispatch
right. The immutable row still records the exact certificate instance/origin for
audit. Concurrent twins serialize on the row: the loser waits, ends its aborted/no-op
transaction, rereads state in a fresh transaction, and never executes rate/budget/
audit admission. The route changes `admitted -> dispatching` durably before calling the
vault. From that point no automatic code path may retry credential use or vendor
dispatch.

Failures proved before invoking `kb_vault_key_use` may transition `admitted -> failed`
and settle zero. After the call is invoked, every non-OK status is conservatively
`uncertain` and retains/charges the full reservation because the existing enum does not
expose whether the durable intent committed. This avoids inventing a safe retry across
the P7 commit/callback crash gap. Crash recovery does not compose around the existing global
`db2_org_budget_settle_expired` sweep. Add one request-correlated SECURITY DEFINER
recovery operation that scans an indexed lease/state key with `FOR UPDATE SKIP LOCKED`
in fixed batches of at most 100. For each exact `(team,request_id,origin)`, it atomically
changes stale `admitted` (which proves no vault invocation) to `failed` and settles
zero, or stale `dispatching` to `uncertain` and settles that exact reservation at its
maximum; it changes the matching token audit to the corresponding terminal or
`indeterminate` state in the same locked transaction. Normal settlement takes the same
dispatch-row lock and state predicate, so recovery cannot double-settle a live owner.
Include a production startup plus periodic runner, coordinated by a singleton advisory
lock and bounded batches. It never touches a key or network. Process death followed by
restart must converge without operator DML.

For an authenticated complete vendor response, settle dispatch, budget, and token
audit in one transaction. Success records actual usage/cost. A proven provider denial
with no usage settles at zero only when the transport proves the complete response was
authenticated and the provider class is explicitly non-billable; all other provider
errors conservatively charge the maximum. Any impossible/overflowing usage charges
the maximum and records integrity failure.

## Vault-sign/network split

Do not call `kb_bedrock_dispatch_buffered` from the plaintext callback. Split the P6
surface into two ownership-safe operations:

- an internal builder accepts the opaque authorized target, pre-serialized canonical
  body, and borrowed length-bearing AWS credential views (no `strlen` contract); it
  creates one initialized, owned `kb_bedrock_wire_request_t`
  containing the body, target-derived host/path, payload hash, access-key identifier,
  session token if any, and signature—but never the secret access key;
- a dispatcher accepts only the opaque target plus that owned signed request and does
  TLS/network/response decoding without any raw credential parameter.

The P7 callback uses a length-aware, allocation-free scanner over the borrowed locked
arena to strictly parse one bounded vault plaintext JSON object containing
`access_key_id`, `secret_access_key`, and optional `session_token`, rejects duplicate
or unknown keys, rejects escapes rather than decoding into heap strings, accepts a
non-NUL-terminated buffer, points offset/length credential views into the locked
arena, builds the signed owned request, and returns. Any unavoidable temporary is
allocated inside the locked nondumpable arena and cleansed before return; ordinary
cJSON is forbidden for credential plaintext. The existing P7 cleanup completes before any DNS, socket,
TLS, or write. Clear the signed request on every exit, including authorization and
transport failures. Deprecate the combined raw-credential dispatch entry point from
production kb callers; pure P6 tests may retain an internal test wrapper.

Delete and replace the unused `kb_egress_admit_dispatch` scaffold rather than calling
it: its caller-supplied `cred_slot`/`reserve_max`, rate-before-later-failure behavior,
raw dispatch callback, and settle-at-reserve semantics are not an authority boundary.
The new route can reach only the combined DB admission and split signed-wire APIs.

Extend the transport result with a conservative `vendor_bytes_possible`/authenticated
completion classification. A complete success can settle actual usage. Once vendor
bytes may have been written, timeout, EOF, cancellation, framing error, or local crash
is `uncertain` and keeps the maximum charge. Definite pre-write failures are `failed`
and may settle zero, but never redispatch under the same request ID.

## WORM witness release gate

P7's local WORM intent is necessary but the off-host witness/outbox is not yet landed.
Add one explicit egress readiness predicate/configuration seam. Hardened/production
org-key egress is unconditionally release-disabled in this slice; there is no override
in a production build and P2b-a is classified integration-live, production-pending.
The CT260 development gate may enable a conspicuously named lower-level test override
only with a real custody/HWM provider and must emit a startup warning plus a WORM
event, but that run is not represented as production release validation. File/mock
custody remains forbidden. The remaining P7 witness slice must implement the real
predicate and run a second CT260 hardened-build gate before the feature is called
production-live; it replaces the predicate implementation, not the route contract.

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
- Identity: issuer+serial—not CN—owns authorization while `(team,request_id)` owns
  idempotency; primary admission rejects unknown/revoked/mismatched fingerprint and
  issuer/serial even when the listener cache says active; same CN/different serial is
  distinct; same serial/different issuer is distinct; transport/actor confusion and
  worker-thread residue deny. An uncertain request retried after cert renewal within
  the same billing team produces no second P7 intent or vendor send.
- Atomic admission: rate increment rolls back on budget/audit/dispatch failure; budget
  rolls back on later failure; exact replay never increments, reserves, audits, signs,
  or sends; conflicting replay denies; concurrent twins produce one owner.
- Pricing: unpriced/pointer change/overflow/zero ceiling deny; maximum reservation is
  conservative; actual settlement uses pinned version; over-ceiling usage records the
  full liability and activates the team/model fence before another admission.
- Vault split: allocation-free plaintext parser is strict over non-NUL buffers and
  rejects escapes/duplicates; callback performs no network; secret key is
  absent from owned request after callback; arenas and all copies cleanse under
  ASAN/UBSAN/leak checks; seal and HWM failures never send.
- State/failpoints at every boundary: after admission, after dispatching, after P7
  intent, after signed-request build, before first byte, after partial write, after
  authenticated headers/body, and before settlement. No failpoint permits a second
  dispatch. Startup and periodic request-correlated recovery make stale work
  uncertain/content-free and settle only the matching reservation/audit.
- HTTP workers: slowloris/read timeout, malformed Content-Length, body octets without
  length, bodyless `/v1/enroll/ca` regression, heartbeat/renew framing, saturation,
  shutdown, SIGPIPE, cancellation, and response-size bounds.
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
5. Prove unentitled, wrong-team/project, disabled/unpriced binding, revoked cert
   changed on the primary after the listener prefilter, cert-renewal replay,
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
- Off-host WORM outbox/witness implementation and hardened production-release gate,
  operator reconciliation of genuinely ambiguous vendor outcomes, and compromise
  fencing beyond the P2b overage fence (remaining P7). Startup/periodic stale-dispatch
  recovery is included here, not deferred.
- P8 thin-client certificate presentation/runtime integration.
