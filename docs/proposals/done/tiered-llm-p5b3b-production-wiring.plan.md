# P5-B3b production startup, route, and server-auth wiring

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE — implemented, adversarially reviewed, and exact-commit validated on CT260.
- **Parent:** `tiered-llm-p5-oidc-control-plane.plan.md`, P5-B.
- **Depends on:** P5-B3a and its B2a/B2b/B2c prerequisites.
- **Followed by:** P5-B3c real two-node topology and adversarial outage/revocation closeout.

## Boundary

Enable the B3a read-only health exchange in the production kb and server request paths without
adding a management mutation. This packet owns three composition seams only:

1. one process-wide kb management runtime that constructs and owns the B2a provider, B2c
   lifecycle, server TLS trust, authority TLS trust/pins, and status verification key;
2. `GET /v1/servers/{id}/health?team=<positive>` invoking the live B3a exchange for the
   authenticated request actor; and
3. a narrow server generic-auth exemption that accepts a verified dedicated management-profile
   mTLS leaf only for `POST /v1/management/challenge` and `GET /v1/management/health`.

No action route, mutation, operator JWT, OIDC propagation, pooling, enrollment redesign, new
certificate/signing path, or topology claim belongs here.

## Kb runtime ownership and configuration

Add `src/kb/kb_management_runtime.{h,c}`. The singleton is initialized after DB2 and vault
startup synchronization but before `kb_http_start()`, and is stopped after `kb_http_stop()` has
joined request workers but before DB2/provider teardown. It owns, in destruction order, the
registered route callback, B2c lifecycle, B2a provider, copied CA PEM, pins, and public
verification key. No request may observe a partially initialized or already-closing runtime.

The singleton has one explicit mutex-protected state machine:

`DISABLED` (the whole packet absent) → `RECONCILING` → `READY`, with `RETRY_WAIT`,
`READY_DEGRADED`, `TERMINAL`, and `STOPPING` side states. A syntactically configured runtime
registers its route callback before HTTP starts, but that callback returns typed `UNAVAILABLE`
unless state is `READY` or `READY_DEGRADED`; it never treats pending startup as disabled. Static
configuration failure aborts startup before registration. A retryable failure before first
success enters `RETRY_WAIT`; after at least one successful reconcile it enters
`READY_DEGRADED`, where B3a still independently rejects an expired or unusable active leaf.
Integrity/denied/invalid lifecycle outcomes enter `TERMINAL`, whose callback remains a stable
503 until restart/operator repair. `STOPPING` rejects new references; HTTP is stopped and joined
before lifecycle/provider memory is cleansed and freed. A borrower that observes `STOPPING`
returns `UNAVAILABLE` without dereferencing the lifecycle/provider adapters; unregister blocks
new borrows and waits for the in-flight count to reach zero before any owned object destruction.

Configuration is operator/root supplied through a single explicit environment packet:

- `AIMEE_KB_MGMT_INSTALLATION_ID` (exact 32 lowercase hex),
  `AIMEE_KB_MGMT_CUSTODIED_CA_DIR`, and `AIMEE_KB_MGMT_BUNDLE_DIR`;
- `AIMEE_KB_MGMT_WORKLOAD_HELPER`, `AIMEE_KB_MGMT_WORKLOAD_JWKS`,
  `AIMEE_KB_MGMT_WORKLOAD_PROOF_SPKI`, `AIMEE_KB_MGMT_WORKLOAD_ISSUER`, and
  `AIMEE_KB_MGMT_WORKLOAD_AUDIENCE`;
- `AIMEE_KB_MGMT_SERVER_CA_FILE`;
- `AIMEE_KB_MGMT_STATUS_ENDPOINT`, `AIMEE_KB_MGMT_STATUS_CA_FILE`,
  `AIMEE_KB_MGMT_STATUS_LEAF_PIN`, and optional
  `AIMEE_KB_MGMT_STATUS_SECONDARY_LEAF_PIN`;
- the existing cross-side `AIMEE_MGMT_STATUS_KEY_ID` and
  `AIMEE_MGMT_STATUS_PUBLIC_KEY` (exact 32-byte lowercase-hex Ed25519 key).

Those last two names are deliberately the same public status-authority configuration already
consumed by the server/B3a codecs; B3b does not invent a second key namespace or any signing-key
input.

All variables absent means disabled and the live route returns 503. Any partial packet, malformed
value, unsafe/non-absolute path, unreadable static trust file, or invalid PEM/key/pin is a startup
configuration error: never silently disable or fall back to the old heartbeat row. Operational
provider/lifecycle/reconciliation outages do not take down unrelated kb functions; they leave the
management runtime explicitly unavailable and retry under bounded backoff. Integrity, denied,
and invalid lifecycle outcomes are terminal for management until operator repair/restart rather
than silently retried. CA files are bounded regular files opened through the existing checked
root-file primitive (`O_NOFOLLOW|O_CLOEXEC`, ownership/mode checks) and copied into runtime-owned
memory. The workload provider retains its existing checked-file/helper policy. Fixed maximum
token age and helper timeout remain within the B2a constructor limits rather than adding loosely
validated knobs.

The runtime performs initial and periodic bounded B2c reconciliation so enrollment and renewal
are live. `kb_main` drives a nonblocking runtime tick from its existing service loop: healthy
state reconciles every 60 seconds; retryable construction/reconcile failures back off
5,10,20,40,80,160, then 300 seconds and remain capped at 300. Each reconcile receives absolute
wall deadline `now+30`; there is no per-request reconcile and no background thread. Only one
reconcile runs at a time, and route bundle loads serialize through the lifecycle's existing
mutex. The request callback uses a 15-second absolute
monotonic deadline, the B3a primary snapshot and lifecycle adapters, untrusted server connector,
dedicated authority connector, wall and monotonic clocks, and locally held public verification
key. It never holds the singleton mutex, a DB transaction, or an environment pointer over network
I/O. Shutdown unregisters the callback and waits for in-flight calls before freeing owned state.
Reconcile scheduling is part of B3b; real outage/renewal topology and restart proof remain B3c,
while an expired/unavailable leaf already fails closed in B3a.

## Live kb route and generic authorization

Refactor `kb_http_servers` to expose a process-wide health-handler registration seam with
single-owner register/unregister and in-flight reference counting. Registration and
unregistration occur only while the HTTP listener is stopped. Dispatch borrows the handler under
the seam mutex, increments its in-flight count, releases the mutex for the full network call, and
decrements/signals on return; unregister first blocks new borrows and waits on a condition
variable without holding runtime/lifecycle/DB locks. The production runtime is its only owner;
tests may inject a handler before requests begin. For exactly
`GET /v1/servers/{id}/health`, parse the existing positive `team` parameter, require
`kb_reqctx_actor()` to be authenticated, and invoke the registered live handler. Do not perform
the old direct `db2_server_registry_get()` heartbeat read and do not manufacture an owner in
auth-off mode.

Map typed B3a results without string inspection: `OK→200`, `NOT_FOUND→404`, `DENIED→403`,
`CONFLICT→409`, `UNAVAILABLE→503`, `INTEGRITY→502`, `INVALID→400`. Build bounded exact JSON and
escape no dependency-controlled prose. Method mismatches remain 405. The registration seam is
consulted only by this exact health path; `/v1/servers` list behavior and all other routes remain
on their existing direct paths, with a regression test proving the list callback cardinality is
zero.

On aimee-server, add a pure predicate for the dedicated management transport. It returns true
only for the exact two method/path pairs above and a TLS peer whose already chain-verified leaf
has the management profile and exact management-client CN. In `handle_conn`, evaluate this after
`server_tls_peer_identity()` plus `server_tls_peer_cert()` have extracted the verified leaf and
immediately before the generic DB1 `pki_cert_check`, required-mTLS ramp accounting, transport,
bearer, bootstrap-bearer, and capability gates. The classifier is three-way: `NOT_MANAGEMENT`,
`ALLOW_MANAGEMENT`, or `DENY_MANAGEMENT`.

- either exact management route with no valid profile+CN returns 401 before generic cert/bearer
  fallback; a management-profile leaf on any non-management route returns 403 before generic
  roster/bearer fallback;
- `ALLOW_MANAGEMENT` sets a separate `management_authenticated` flag, bypasses the unrelated
  generic client-roster check and bearer
  requirement, because its online per-request authority is the nonce/staple verifier;
- it does not count toward the optional→required generic mTLS ramp, satisfies the transport check
  only for the matched request, bypasses the one-time bootstrap-bearer gate only for the matched
  request, and receives a zero generic capability mask (the two routes themselves require zero);
- the existing handlers still require the profile/CN, bind issuer+serial+fingerprint and channel,
  validate the fresh signed staple, consume the nonce, and advance the durable HWM;
- `/v1/management/action` remains outside the lane and fail-disabled; no bearer or generic-roster
  identity can broaden the management identity.

The exemption does not treat mere TLS presence, CN alone, a bearer, or a non-management leaf as
management authentication. Required-mTLS promotion and generic client behavior remain unchanged.
The current native TLS listener is the available request-path seam for B3b; a separately bound
management listener and its real certificate/CA topology are explicitly B3c acceptance work.
Server `AIMEE_SERVER_ID` and public status-key parsing already belong to the shipped P5
foundation handlers and remain unchanged here; B3c proves the cross-process values match.

## Tests and validation

Add focused runtime tests for all-absent disabled configuration, partial/malformed fail-closed
configuration, secure bounded file reads, constructor/reconcile failure cleanup, retryable
backoff versus terminal state, periodic renewal, successful dependency composition, callback
registration, typed result propagation, concurrent shutdown, and secret/public-buffer cleansing.
Add kb route tests for missing actor/config/team, every typed
mapping, exact target/team/actor forwarding, method mismatch, and proof that the stale heartbeat
row is no longer returned.

Add pure and request-path server tests covering the exact management method/path/profile/CN
matrix; bearer-free success through the seam; denial on a generic route; wrong profile/CN;
bootstrap-bearer independence; generic roster behavior unchanged; and challenge/health handler
rechecks still enforced. Run existing server HTTP/TLS/management nonce/HWM and kb auth/route plus
B2a/B2c/B3a suites. Run production server/kb/status-authority builds, fresh ASAN/UBSAN focused
tests, and CT260 exact-commit production/focused gates.

Update `api/openapi-v1.yaml` for the live kb route and regenerate the checked generated OpenAPI
header through the normal generator; route/OpenAPI inventory checks must remain clean.

B3c retains the real two-node server↔kb↔authority topology; real startup files and helper;
restart/HWM continuity; revoke between challenge and issuance; server and caller revocation on a
live keep-alive session; DNS rebinding; KMS/helper/seal/authority/DB outage; and wire-level proof
that a management leaf cannot use generic server routes.

## Merge gates

Plan roundtable, delegated implementation, local production/focused/sanitizer validation, CT260,
adversarial full-branch roundtable, all CI, PR merge to `testing`, delivery-table update, and an
aimee memory are mandatory. Any ambiguous auth/configuration state fails closed.

## Completion evidence

The production kb runtime, live health route, and exact-route server authentication seam landed as
one composition slice. Independent review caught and closed set-empty configuration, initial
static-constructor cleanup, and legacy list-query compatibility defects. The adversarial branch
roundtable then produced one genuinely valid additional boundary: reject non-canonical server IDs
at the HTTP route rather than relying only on B3a's downstream token validation. The convergence
roundtable aligned after that fix and found no remaining evidence-backed code blocker.

Production `server`, `kb`, and `status-authority-core` builds; the runtime, route, server HTTP,
B3a exchange, and authority-client focused suites; fresh ASAN/UBSAN runs; OpenAPI conformance and
route inventory; and a clean-archive exact-commit CT260 gate all pass. B3c retains the distinct
management listener and real two-node revocation/outage wire matrix.
