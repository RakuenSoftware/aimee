# P5-C3 management-action composition

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** complete; plan review converged in jobs 8803 and 8805–8808, implementation review
  converged after jobs 8816 and 8819–8821.
- **Parent:** `tiered-llm-p5-oidc-control-plane.md`, §§2–3.
- **Depends on:** P5-A/B3, C1a/C1b/C1c, and C2a–C2d.
- **Followed by:** P5-D console/OIDC propagation.

## Boundary

Enable exactly one end-to-end, single-server management mutation: `agent.enable` and
`agent.disable`. The kb route accepts one bounded canonical request, commits the C1c primary/WORM
intent before obtaining a token or sending a network byte, asks the isolated C2d authority to issue
the exact admitted correlation/JTI, and performs the B3 nonce/staple/action exchange on one pinned
mTLS session. The server verifies the C2c JWT, nonce-bound action staple, and primary-linearized
revocation checkpoint; then consumes the JTI durably in DB1 before the first action side effect,
applies its existing `remote_writes` and capability policy, dispatches the existing agent action
once, and records the propagated
operator identity in its local WORM audit. When the kb survives through outcome commit it appends
one terminal C1c outcome; a crash can leave the already-defined intent-only unresolved state, which
is never automatically redispatched.

This slice does not add bulk/fanout, arbitrary paths or bodies, config mutation, generic proxying,
automatic retries/recovery, console ACL/UI/OpenAPI, or new OIDC claims. Those remain P5-D or later
work. An intent-only or indeterminate record is terminal for automatic dispatch.

## Canonical kb request and route

Add `POST /v1/servers/{server_id}/actions?team=<positive decimal>` to the authenticated kb API.
The query must contain exactly one `team` member, no other member, and one canonical positive
decimal spelling: `[1-9][0-9]*` within signed 64-bit range. Reject duplicates, empty values,
leading zeroes/signs, separators, percent-encoded alternates, and unknown members.
The body is an exact JSON object with exactly two string members. `action` is exactly
`agent.enable` or `agent.disable`. `agent` is 1–63 bytes from `[A-Za-z0-9._-]`; this intentionally
needs no JSON escaping. Reject duplicate, unknown, missing, non-string, embedded-NUL, control,
overlong, or noncanonical values. Parse into a fixed-capacity structure, then emit exactly
`{"action":"agent.enable","agent":"NAME"}` or
`{"action":"agent.disable","agent":"NAME"}`, with that member order and no whitespace. This
byte sequence is both the SHA-256 journal input and the complete server body. Never hash the
caller's raw JSON spelling.

Require the request actor to be authenticated. C1c remains the authorization authority for
platform-admin/team-lead access and target/local certificate snapshots. Initialize one operation
with a configured canonical issuer, current FINAL C2b `kid`, 90-second-or-shorter TTL, and the
runtime's active local installation id; call `intent_start` before token IPC, server connect,
challenge, status issuance, or action bytes. A confirmed exact replay returns conflict/unresolved
and never dispatches. Ambiguous intent commit is retried only with the same operation tuple until
the C1c exact-read result proves committed or unavailable; it never generates a replacement id.

Register the action callback beside the existing health callback with the same borrow/unregister
shutdown discipline. Extend the management runtime configuration only with the fixed token-authority
socket metadata and token issuer/kid/TTL needed for C1c; reject partial packets. The ordinary kb
continues to hold no signing root or generic signing input.

## One-session dispatch protocol

Use one absolute monotonic deadline and one pinned B3 mTLS session. Load the active local client
certificate bundle and primary target snapshot A; require byte equality with the committed C1c
intent. Open to snapshot A's registry endpoint with connect-time address policy, no redirects, and exact
target issuer/serial/fingerprint pinning. Then, in order:

1. request an action-purpose challenge with zero body at the distinct exact route
   `POST /v1/management/action/challenge` on the same connection; this stores purpose
   `management.action.v1`, while the existing health challenge remains purpose-bound to
   `management.health.v1`;
2. obtain a status-authority signature for purpose `management.action.v1`, bound to that nonce,
   local peer certificate, target server id, and current revocation generation;
3. load primary snapshot B and require exact A/B equality for server id, endpoint, active/enrollment
   state, management issuer/serial/fingerprint, plus equality to the committed C1c tuple and opened
   TLS peer; require the returned staple to match the same tuple. C2d's private admission envelope
   is deliberately neither exposed nor compared by ordinary kb;
4. call C2d with only correlation/JTI and accept only exact `OK`; and
5. send exactly one canonical `/v1/management/action` request with
   `Authorization: Bearer <C2d-JWT>` and `X-Aimee-Management-Status: <B3-JSON-staple>`, plus the
   canonical action body. The complete request must fit `SHTTP_READ_MAX`; header extraction rejects
   overlong or duplicate security headers rather than truncating them. The JWT must be nonempty and
   at most `KB_MGMT_TOKEN_WIRE_MAX`; the staple must be nonempty and at most
   `KB_MGMT_STATUS_JSON_MAX`; CR/LF and embedded NUL are forbidden.

The action server performs a second, direct online checkpoint against the existing dedicated
revocation-status authority after validating the staple and immediately before accepting the JTI.
This is not a lease. The server uses its enrolled server→kb `clientAuth` identity over pinned mTLS;
redirects, proxies, replicas, and cached replies are forbidden. The authority's one primary
transaction reads the current global generation and the named kb management certificate's current
revocation state together, linearizable with the transaction that atomically commits a revocation
and increments that generation.

The server runtime packet is all-or-none and required whenever the management listener is enabled:
`AIMEE_SERVER_MGMT_STATUS_ENDPOINT`, `AIMEE_SERVER_MGMT_STATUS_CA_FILE`,
`AIMEE_SERVER_MGMT_STATUS_LEAF_PIN`, optional
`AIMEE_SERVER_MGMT_STATUS_SECONDARY_LEAF_PIN`, `AIMEE_SERVER_MGMT_STATUS_CLIENT_CERT`, and
`AIMEE_SERVER_MGMT_STATUS_CLIENT_KEY`. Pins are lowercase 64-hex and file values are root-owned
absolute paths. The endpoint is HTTPS only; the existing strict endpoint parser, connect-time address
policy, hostname/SNI validation, exact leaf-pin rotation rule, root-file checks, no-redirect client,
one five-second absolute deadline, and joined shutdown ownership apply. Partial/invalid packets make
management-listener startup fail closed.

Extend the dedicated authority listener with exact `POST
/v1/management/action-checkpoint`; the existing `/v1/management/status` wire is unchanged. Both use
HTTP/1.1, exactly one Host/Content-Type `application/json`/canonical Content-Length, no transfer
encoding, upgrade, expect, proxy, interim response, surplus byte, or reuse. The existing status body
retains its 1024-byte cap; the checkpoint body has a separate 4096-byte cap.
The exact checkpoint request member order is
`version,purpose,nonce,caller_issuer_b64,caller_serial,caller_fingerprint,target,staple_generation,staple_sha256,correlation_id,jti,request_sha256`.
`version` is exactly the JSON string `"1"`. All numeric fields are canonical decimal strings; the issuer is canonical unpadded base64url of its
exact bytes; nonce is canonical unpadded base64url; digests/ids are lowercase 64-hex. Unknown,
duplicate, missing, reordered, overlong, embedded-NUL, or noncanonical fields are invalid.

The authority admits only a verified mTLS peer whose issuer/serial/fingerprint is an active,
unrevoked, unexpired `p5-server-client` enrollment for exactly `target`. One primary transaction
locks/reads that server enrollment, the named kb `p5-kb-management` enrollment, its current revoked
state, and the singleton generation. Replica/read-only access is unavailable. The exact response
member order is
`version,domain,request_sha256,revoked,generation,issued_at,expires_at,key_id,signature`, where
`version` is exactly `"1"`, `domain` is exactly `management.action.checkpoint.v1`,
`request_sha256` hashes the exact canonical checkpoint request, `revoked` is JSON boolean, numeric
fields are decimal strings, and signature is canonical Ed25519 base64url. Its signed transcript is
domain `management.action.checkpoint.v1` plus every response field except signature; the request
digest thereby binds every request field. Authority time fixes `expires_at = issued_at + 5`; the
server requires `issued_at <= server_now < expires_at`, that exact five-second lifetime, and no
future-issued tolerance. Success, including an authoritative `revoked=true` or a newer current
generation, is HTTP 200 so the server verifies and rejects from signed data. Syntax/version/shape
failure is exact HTTP 400 `{"error":"bad_request"}`; inactive, expired, revoked, wrong-target, or
mismatched authenticated server enrollment is exact HTTP 403 `{"error":"denied"}`; a staple
generation greater than the primary generation or protected signer-use conflict is exact HTTP 409
`{"error":"conflict"}`; primary/read-only/storage/custody/signing unavailability is exact HTTP 503
`{"error":"unavailable"}`. No error contains admission data, and every non-200 fails closed.

The server verifies the
pinned authority transport, signature, all bindings, freshness, and its durable highest-seen
generation, and requires `revoked=false` plus `checkpoint.generation == staple.generation`.
Unavailable, ambiguous, stale, mismatched, or rollback responses fail closed before JTI consume.
No network operation, JWKS refresh, status lookup, retry, sleep, or queueing may occur between a
successful checkpoint verification and DB1 JTI consumption.

No request may be automatically retried after any action-request byte is written. The client
transport must expose `NOT_SENT`, `SENT_RESPONSE`, and `SENT_AMBIGUOUS` rather than collapsing a
post-send failure into ordinary unavailability. Challenge/status failures are pre-dispatch and may
close the session, but this synchronous request does not redrive them.

C2d `ALREADY_USED`, `COMMIT_AMBIGUOUS`, or any non-OK result is terminal pre-dispatch unresolved.
Append `failed/local_failure` with no status/response digest after any post-intent failure proven to
precede action-request bytes. If the action might have reached the server, append
`indeterminate/transport_ambiguous`; never label it failed. Outcome commit ambiguity is resolved
only by exact C1c outcome replay with the identical tuple.

## Server authority and side-effect ordering

Extend the dedicated management listener allowlist to admit only the action challenge and `POST
/v1/management/action`; they remain forbidden on every other lane/profile. Require the verified
`p5-kb-management` client certificate, a valid unconsumed challenge and a fresh
`management.action.v1` staple for this request. Extract bounded authorization/staple headers into
request-scoped state and clear it at the end of every request.

Replace the C1a endpoint stub with a closed composition that:

1. canonicalizes and hashes the exact allowed action body;
2. verifies the JWT through the production authenticated C2c cache, including one bounded
   unknown-`kid` refresh;
3. checks issuer, target audience/server id, capability `remote_writes`, request digest, peer CN,
   correlation/JTI, and time claims;
4. verifies/consumes the nonce-bound staple;
5. obtains/verifies the linearizable online revocation checkpoint above;
6. immediately durably consumes the JTI in DB1 as the replay/at-most-once barrier;
7. reads the server's own process-lifetime `remote_writes`, requires exactly `full`, and derives the
   action capability at the final dispatch seam; and
8. appends local management intent, calls only the existing `agent.enable` or `agent.disable`
   handler, then appends the local outcome under the JWT actor.

The checkpoint primary read is the management-certificate authorization linearization point: a
revocation committed before it is observed and rejects the action, while a revocation committing
after it is ordered after this action's certificate authorization. The process-lifetime
`g_remote_writes` policy is initialized once and has no in-process writer;
configuration changes take effect only on restart, which closes the request. Read it at the final
dispatch seam and prove by test/plant check that no live writer exists and that shutdown prevents
the old process from reaching the handler. If a future live writer is added, it must serialize
policy changes against this seam.

JTI consumption is the durable replay/at-most-once barrier, not a claim that a response or complete
audit is inevitable. Any failure before consume has no side effect. After consume, replay cannot
invoke the handler again, but crash or connection loss may leave the result indeterminate. Server
audit failure before the handler fails closed. The existing enable/disable wrapper must establish
that handler success means the atomic config replacement committed and that handler error is either
proven pre-effect or reported as effect-unknown. Outcome-audit failure after a committed handler is
always effect-unknown and never re-runs the action.
The operator actor comes only from the verified JWT and must never fall back to `operator`, console
admin, or the kb certificate principal.

Every server response has exactly `Content-Type: application/json`, one canonical Content-Length,
no transfer encoding/interim/surplus bytes, and uses an exact closed object with member order
`{"result":"succeeded|denied|failed|indeterminate","effect":"applied|none|unknown"}`. Only
`succeeded/applied`, `denied/none`, `failed/none`, and `indeterminate/unknown` are valid pairs.
Their only valid HTTP statuses are respectively 200, 403, 500, and 502; every status/body mismatch
is protocol-indeterminate. Connection close after the complete exact response is permitted and the
action session is never reused.
Map them respectively to `succeeded/remote_success`, `denied/remote_denied`,
`failed/remote_failure`, and `indeterminate/protocol_failure`. A missing, malformed, inconsistent,
or effect-unknown response is indeterminate even when its HTTP status is 4xx/5xx; transport
ambiguity maps to `indeterminate/transport_ambiguous`. Hash only the exact bounded response body
received and store no JWT, staple, request body, endpoint, certificate, or provider text in
DB2/WORM records.

## Verification

Focused unit tests cover strict canonical request parsing, digest stability, callback lifetime,
intent-before-token/network ordering, exact replay refusal, every C2d result, pre-send versus
post-send transport classification, outcome mapping/replay, output clearing, deadlines, and
shutdown with an in-flight action. Server tests cover lane/profile isolation, exact headers/body,
staple purpose/nonce, C2c unknown-kid refresh, all claim bindings, JTI-before-side-effect ordering,
32-way replay, `remote_writes=off|data|full`, actor propagation, audit failures, and exactly one
enable/disable invocation. Include process crashes after JTI consume, after config replacement, and
before each audit/response boundary; an intent-only kb crash must remain non-redrivable. Fuzz the
bounded action envelope and management header parser; run ASAN/UBSAN.

The real PostgreSQL 17 gate proves one C1c intent/outcome and WORM event each, one C2d key-use event,
one DB1 JTI consumption, actor/team isolation, stale/revoked snapshot denial, outcome ambiguity,
and no automatic redrive. CT260 runs kb, token authority, status authority, and server as distinct
process identities against real PG17 and the production mTLS/JWKS/custody paths. It enables then
disables a real agent, verifies the propagated actor in server audit, proves `remote_writes=off`
blocks mutation, revokes between challenge and action, injects a post-send connection loss, and
proves replay/parallel requests never cause a second side effect.

Run production builds, focused sanitizer/fuzz gates, schema/role/isolation checks, exact-head CT260
validation, and an adversarial full-branch roundtable. Incorporate every genuinely valid finding,
converge, merge through the normal PR flow, update delivery status, and store the next final tally.

## Delivered outcome

The completed slice composes the C1/C2/B3 authorities into the bounded action route and the
dedicated server management lane. It preserves one pinned mTLS session across the action challenge
and final request, binds both certificate snapshots and the signed status generation exactly to the
committed intent, obtains the isolated token only after those checks, and retains the transport's
not-sent versus ambiguous classification. The server validates the token/staple/checkpoint chain,
durably consumes the JTI, rechecks process-lifetime write policy at the final seam, and calls a
commit-aware enable/disable core whose nonzero result is proven pre-effect. Shutdown closes the
action gate and joins the complete admitted request lifetime before checkpoint-client teardown.

Validation covered production server/status-authority builds, focused unit and sanitizer suites,
the three action/checkpoint fuzz targets, boundary/schema/tier/tenant guards, an exact-head CT260
build, and the full real-PostgreSQL 17 isolation/concurrency gate. Review-discovered fixes included
same-session HTTP framing, exact staple-generation equality, byte-length-preserving request
dispatch, credential cleanup on failed loads, commit-aware effect reporting, and full-request
shutdown ownership. Equal highest-seen checkpoint generations are explicitly regression-tested as
valid while rollback remains denied.
