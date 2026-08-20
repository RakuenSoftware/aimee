# P5 implementation plan — security-closed control-plane slices

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

> **Archived complete (2026-07-26).** The audit found the scoped deliverables shipped,
> superseded by the current implementation, or fully represented by completed child slices.

- **State:** DONE — delivered scope archived 2026-07-26.
  complete; P5-B composition is the next implementation slice.
- **Proposal:** `tiered-llm-p5-oidc-control-plane.md`.
- **Existing substrate:** P1 composite identity/OIDC/JWKS verification, P7 custody and
  PostgreSQL WORM append, the P8a per-request enrollment revocation seam, and the
  current registry/management helpers. Those helpers are scaffolding, not evidence
  that the control plane is safe to enable.

## Why P5 is split

The proposal's guarantees are coupled at the point where kb can cause a server
mutation. A reverse TLS client or JWT verifier is not independently safe if it can
reach a write before endpoint pinning, next-request revocation, durable replay
rejection, propagated actor identity, and the authoritative kb WORM intent all exist.
P5 therefore lands as four ordered, independently security-closed slices. No slice
before P5-C exposes a management write.

1. **P5-A — authoritative registry and role-separated certificate topology.** Build
   the primary-backed enrollment/heartbeat/lookup substrate and prove two nodes use
   distinct `clientAuth` and `serverAuth` leaves. The management action route remains
   fail-closed.
2. **P5-B — pinned reverse mTLS and nonce-bound revocation status.** Add connect-time
   address revalidation/IP pinning, enrolled server-certificate pinning, a dedicated
   status-authority credential, and the server challenge/staple protocol. Enable only
   a read-only health probe.
3. **P5-C — operator authorization and audited single-server actions.** Add kb token
   minting, authenticated generation-bearing JWKS lifecycle, durable server-local
   `jti` consumption, primary kb WORM intent/outcome, and the real `remote_writes` /
   capability-gated server action. This is the first slice allowed to mutate.
4. **P5-D — console/OIDC propagation and fleet UX.** Preserve the composite OIDC or
   break-glass actor through console→kb, add the server-scoped console allowlist and
   fleet drill-down, and finish OpenAPI/route coverage. Bulk orchestration remains a
   later proposal.

P7 supplies custody, key-use admission, anti-rollback building blocks, and the WORM
ledger. P5 owns the revocation-generation schema, online status protocol, management
token/JWKS contract, durable `jti` store, and management audit state machine.

## P5-A — exact next slice

**Delivery:** complete. The primary-backed pending/finalize/list/snapshot/heartbeat
state machine, role-separated two-key issuance, server heartbeat worker, hard-disabled
management route, and CT260↔CT262 EKU topology matrix are implemented and validated.

### Security boundary

P5-A may enroll and inventory server identities and accept a content-bounded
heartbeat. It must not dispatch a kb→server management request. The existing
`/v1/management/action` scaffold returns `503` unless the later P5-C complete-control
gate is compiled and configured; setting only the legacy `AIMEE_MGMT_*` environment
variables cannot enable it.

Registry authority is issuer+normalized-serial/fingerprint, never a caller-selected
CN string. The enrollment grant fixes `server_id`, team, owner composite identity,
endpoint, and both certificate roles before redemption. Two independent CSRs and
keys are required: the server→kb leaf has only `clientAuth`; the management-listener
leaf has only `serverAuth`. A role mismatch fails at certificate verification, not at
an application header.

### PostgreSQL state and APIs

- Extend `kb_server_registry` with immutable issuer/serial/fingerprint fields for
  both roles, explicit `pending|active|revoked|expired` state, enrollment operation
  id, and bounded activation expiry. Unique constraints cover `server_id`, each
  `(issuer,serial)`, each fingerprint, and the enrollment operation.
- Add SECURITY DEFINER functions and typed DB2 wrappers for pending creation,
  idempotent two-role finalization, authenticated heartbeat, team-scoped list, and
  one-row management snapshot. Functions set a fixed `search_path`, validate bounded
  inputs, use exact SQLSTATEs, and append the audit row in the same transaction.
- Pending creation derives team and owner from the authenticated operator and a
  mint-time grant; redemption cannot choose them. Retrying the same operation and
  same CSR pair returns the same result. A mismatched CSR, role, endpoint, team,
  owner, or operation id is a conflict and never partially activates a row.
- Heartbeat derives the presented certificate issuer/serial/fingerprint from mTLS,
  updates exactly one matching active row, and distinguishes zero-row denial from a
  successful update. Revoked/expired enrollment, wrong role, wrong server id, and
  cross-team identity are denied.
- The management snapshot is one primary query/transaction returning endpoint,
  management certificate identity, registry state, and enrollment revocation state
  from one snapshot. P5-A does not dial it yet.
- `FORCE ROW LEVEL SECURITY` remains mandatory. Real runtime-role tests cover every
  new definer and direct table access remains unavailable. The SQLite schema is shape
  compatibility only and every production wrapper fails closed without PostgreSQL.

### PKI and process wiring

- Add a strict CA signing profile selector whose only public values are
  `server-to-kb-client` and `kb-to-server-listener`. It ignores CSR-requested EKUs and
  emits exactly one intended EKU plus the existing bounded validity/key constraints.
- Validate CSR proof-of-possession, SAN/identity policy, leaf chain, EKU, issuer,
  normalized serial, and DER SHA-256 before finalization. The two roles must use
  different public keys and certificates.
- Wire a server heartbeat client that presents the enrolled `clientAuth` leaf to
  CT260's existing mTLS listener. Wire a distinct management listener on the second
  node that presents the `serverAuth` leaf, but accepts no P5 action in this slice.
- Do not add TOFU, bearer fallback, dual-EKU leaves, redirects, or shared fleet keys.

### P5-A validation

- Build `server` and the focused registry/PKI/route tests; run lint, schema-sync,
  proposal-link, and route-coverage gates.
- Real PG17 runtime-role gate: pending→active retry, conflicting retry rollback,
  unique identity constraints, wrong-cert heartbeat denial, revoked/expired denial,
  cross-team list/get/heartbeat denial, primary snapshot consistency, and no direct
  table access.
- Two-node integration uses CT260 as kb/PG authority and throwaway CT262 as the
  server. It generates two independent keys/CSRs, obtains role-specific leaves,
  proves server→kb heartbeat and team list, and proves each leaf is rejected in the
  opposite TLS role. Also reject unknown CA, expired leaf, wrong server identity,
  revoked enrollment, and a legacy bearer-only management request.
- Assert `/v1/management/action` remains `503` before parsing JWT, touching the
  network, or writing either audit store. No test-only bypass may appear in the
  hardened artifact.
- Run ASAN/UBSAN on new parsers and certificate metadata paths. Run an adversarial
  branch roundtable over the complete diff and this plan before merge.

### P5-A failure semantics

- PostgreSQL unavailable or not-primary-capable: `503`; malformed enrollment/CSR:
  `400`; unauthenticated/unregistered/revoked certificate: `401`; authenticated but
  wrong team/role: `403`; idempotency mismatch: `409`.
- CA success followed by database-finalization failure leaves only a bounded pending
  operation. No certificate is returned until both roles are durably active; retry
  uses the same operation and CSR digests. Expired pending rows are quarantined and
  auditable, never silently reused.
- A heartbeat failure never changes registry authority fields. A zero-row update is
  a denial, not success.

## P5-B — pinned reverse mTLS and nonce-bound status

**Delivery:** foundation complete. The primary status authority, strict signed
status protocol, endpoint/TLS pinning substrate, server nonce/high-water verifier,
and management-client certificate profile are implemented and validated. The next
composition slice connects them through a dedicated custodial authority and kb
orchestrator on CT260↔CT262; until then only read-only server routes exist and no
management action is enabled.

### Security boundary and delivery shape

P5-B exposes exactly one new operation: a read-only management health probe. It does
not proxy a general path, accept a bearer fallback, mint an operator JWT, consume a
management `jti`, or make `/v1/management/action` reachable. The implementation may
be built internally as transport/status sub-packets, but this slice is not complete
and the health probe is not enabled until both compose end to end.

The online status authority is a separately runnable, narrow service identity. Only
that service can use the `p5-management-status` signing key through the P7 custody
and key-use admission boundary; an ordinary unsealed kb process receives only a
signed staple and cannot read the private key or call an in-process signing helper.
The status key is distinct from the enrollment CA and management-JWT keys. Servers
pin its public key and a bounded key id through deployment/enrollment configuration.

P5-B also closes the remaining caller-credential gap from P5-A: each kb instance
auto-enrolls a distinct `p5-kb-management` `clientAuth` leaf using its already
verified platform workload identity, persists it only in that instance's custody
domain, and renews it with the same identity binding. A third strict CA signing
profile, `kb-to-server-management-client`, permits only that scope and clientAuth
EKU; renewal requires the same platform workload identity, permits only a bounded
overlap, and a revoked leaf cannot renew. The management listener and status
authority accept only that profile. A manually shared fleet credential or a generic
enrollment leaf cannot enable the health path.

### Primary state and revocation transitions

- Add one singleton, primary-backed `kb_cert_revocation_generation` row initialized
  to one. Every authoritative transition that makes an enrollment newly revoked
  increments it in the same transaction as the revocation; retries that change no
  state do not increment it. It is deliberately unrelated to vault seal/key epochs.
- Add a SECURITY DEFINER status lookup that takes the caller certificate's exact
  issuer, normalized serial, and fingerprint plus a bounded target server id and
  purpose. In one primary query it returns the current generation and the target's
  authoritative management fingerprint only when
  the caller is an active, unexpired `p5-kb-management` enrollment and the target is
  an active registry row. Runtime roles receive EXECUTE only, never table access.
- Extend the existing management snapshot to return the same revocation generation.
  kb performs this primary query before each request, including requests on a reused
  TLS connection. Inactive/revoked server enrollment or a lower generation aborts
  before bytes for the protected request are written.
- SQLite mirrors only the table shape. Production wrappers require PostgreSQL and
  fail closed on primary outage, zero rows, invalid state, or generation rollback.

### Canonical status protocol

The management listener provides two fixed routes on required mTLS:

1. `POST /v1/management/challenge` creates 32 random bytes with the OS CSPRNG and
   returns base64url nonce plus an absolute expiry no more than 15 seconds ahead.
   Server state binds the nonce to the verified caller leaf fingerprint, issuer and
   normalized serial, the configured server id, and purpose
   `management.health.v1`. At most 128 outstanding nonces are allowed; expired rows
   are removed before admission and saturation denies the challenge without
   eviction. They are single-use and all outstanding nonces are deleted on restart,
   while the generation high-water survives restart.
2. The kb instance sends the nonce, target server id, purpose, and the target
   management fingerprint from its fresh registry snapshot to the dedicated
   status authority while presenting the same management `clientAuth` leaf. The
   authority derives issuer/serial/fingerprint from that verified mTLS peer, queries
   PostgreSQL primary, requires the requested target fingerprint to equal that same
   query's registry result, and returns a signed staple. Caller-supplied certificate
   identity is never authoritative.
3. `GET /v1/management/health` carries the staple on the same TLS connection. The
   server binds it to the actual peer leaf, atomically consumes the matching nonce,
   verifies the signature/key id, exact target and purpose, issued/expiry window,
   target management fingerprint, and generation `>=` its durable high-water mark
   before returning bounded health JSON. Nonce consumption, verification outcome,
   and high-water comparison/advance use one server-local transaction and one lock
   order. Every verification failure consumes the nonce. Successful higher
   generations are durably advanced before the response; a persistence failure
   denies the request.

The signed bytes are a versioned, unambiguous length-prefixed binary transcript over
`version, key_id, nonce, caller_issuer, caller_serial_norm, caller_fingerprint,
target_server_id, target_mgmt_fingerprint, purpose, issued_at, expires_at,
revocation_generation`; integers are unsigned network byte order. Version is one;
key id is 1..64 token bytes; nonce is exactly 32 bytes; issuer is 1..600 printable
bytes; serial is 1..128 lowercase hex; each fingerprint is exactly 64 lowercase hex;
server id is 1..127 token bytes; purpose is exactly `management.health.v1`; times and
generation are u64, the signature is 64 bytes, and all variable fields carry u32
network-order lengths. The outer wire object is strict JSON containing those exact
fields and base64url-no-pad signature; numeric values are canonical decimal strings;
duplicates, unknown fields, non-canonical encodings, overflow, and trailing data are
rejected. Ed25519 is the initial algorithm and algorithm choice is not accepted from
the request. Clock skew is bounded to two seconds and staple lifetime to ten seconds.

### Reverse transport and endpoint policy

- Parse `https://host[:port]` once, rejecting userinfo, paths, queries, fragments,
  ambiguous IPv6 syntax, zone ids, NUL/control bytes, and invalid ports. The enrolled
  DNS name remains the TLS SNI, hostname-verification name, and HTTP `Host` value.
- Resolve afresh with one `getaddrinfo(AF_UNSPEC)` call at every connection attempt.
  Normalize IPv4-mapped IPv6 to IPv4 and
  reject every candidate in IPv4 loopback, RFC1918, link-local/metadata, multicast,
  unspecified, or IPv6 loopback, `fe80::/10`, `fc00::/7`, multicast, and unspecified
  ranges. Denied candidates are filtered independently (including mixed A+AAAA
  answers); a name is usable only if at least one candidate is permitted. The exact
  returned permitted sockaddr is passed directly to `connect(2)`, never through a
  second hostname lookup.
- After normal CA/hostname/EKU validation, hash the DER peer leaf and constant-time
  compare it with the registry snapshot fingerprint; issuer and normalized serial
  must also match. No TOFU, proxy environment variables, plaintext downgrade, or
  redirects are supported. Any 3xx is a terminal failure.
- Challenge and protected health request share one TLS connection. Before the second
  write, kb re-reads the primary snapshot and refuses changed endpoint/cert identity,
  inactive/revoked state, or generation rollback. Connection pooling may be added,
  but every protected request must obtain a new server nonce and staple and repeat
  the primary server check.

### P5-B validation and failure semantics

- Unit/fuzz/ASAN coverage exercises endpoint parsing and every IPv4/IPv6 boundary,
  mapped-address bypasses, mixed DNS answers, leaf mismatch, strict staple parsing,
  transcript ambiguity, nonce expiry/replay/cross-peer/cross-server reuse, signature
  failure, stale/rollback generations, high-water persistence failure, and 3xx.
- Real PostgreSQL 17 runtime-role tests prove atomic generation increments, no-op
  revoke stability, status denial after revoke, cross-team/target denial, primary
  failure, no direct table access, and management-snapshot consistency.
- CT260 runs PostgreSQL and the separately credentialed status authority; CT262 runs
  the server management listener. The two-node test uses an isolated non-denied test
  address, validates the full challenge→authority→health exchange, then revokes
  the kb management leaf between challenge and status issuance. Issuance is denied,
  and the next complete challenge cycle also fails on the existing TLS connection.
  A revocation committed after a staple has already been issued defines the boundary
  between that in-flight logical request and the next request; P5-B never claims an
  offline signature can observe a later database commit. It also proves DNS
  rebinding/private candidates, wrong EKU/CA/leaf, authority outage, replay,
  rollback, and redirects fail closed.
- Status-authority/primary/network errors map to `503`; unauthenticated or invalid
  peer/staple to `401`; authenticated binding/purpose/target mismatch to `403`;
  replay/expired/conflicting challenge to `409`. No failure reaches a server write,
  and `/v1/management/action` remains unconditional `503`.

Run the hardened server/kb/status-authority builds, focused tests, schema/grant gates,
lint and route coverage, then an adversarial branch roundtable over the full diff.

## Requirements pinned for later P5 slices

P5-C must verify structured JWT claims without a second permissive parser; bind the
token to the actual mTLS peer and target audience; publish signed, non-rollbackable
JWKS generations over the authenticated server→kb channel; refresh once on unknown
`kid`; and fail closed on stale/unreachable JWKS. It persists unique `jti` + expiry in
the server's local database before action, so restart cannot reopen replay. kb writes
a tenant-qualified, correlated WORM intent before network dispatch and an outcome
afterward; an intent failure prevents dispatch and an ambiguous result stays
indeterminate. The target server still enforces its own capability and
`remote_writes` setting.

## Explicit deferrals

Bulk fleet actions, config fan-out/templates, SAML, automatic certificate-rotation
scheduling, and P2b-b model-stream forwarding are outside P5. They cannot weaken the
single-server controls above.
