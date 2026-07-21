# P5 implementation plan — security-closed control-plane slices

- **State:** IN PROGRESS. P5-A is the next implementation slice.
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

## Requirements pinned for later P5 slices

P5-B must resolve every address at connect time, reject public-name→private-address
rebinding including IPv4-mapped IPv6, pin the socket to a validated address, preserve
SNI/Host for the enrolled name, reject redirects, and pin the enrolled server leaf.
Its dedicated online status authority signs a fresh server nonce plus caller
issuer/serial, target server, purpose, expiry, and monotonic revocation generation;
unreachable authority, stale/invalid staple, rollback, or revocation fails closed on
the next request even on a pooled connection.

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
