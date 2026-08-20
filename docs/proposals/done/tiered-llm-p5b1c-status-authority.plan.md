# P5-B1c required-mTLS management-status authority

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** complete; plan and adversarial branch roundtables converged.
- **Depends on:** P5-B1a custody core, P5-B1b fixed-key bootstrap/online DB boundary, P7 KMS signed-HWM custody.
- **Followed by:** P5-B2/B3 workload enrollment and challenge-to-health orchestration.

## Boundary

This slice ships the first online process able to compose the completed status
cores: a dedicated, narrowly linked `aimee-kb-status-authority` listener.  It
accepts one direct status request from a pre-enrolled per-instance KB management
client, proves that peer's dedicated certificate profile, resolves the caller and
target against the primary, enters the P7 custodial use boundary, and returns the
existing signed 13-field status document.  It does not enroll or renew client
certificates, dial a registered server, implement challenge/health orchestration,
accept management actions, propagate OIDC identity, or add any ordinary KB route.

The authority is a separate binary and deployment identity.  The ordinary
`aimee-kb` and `aimee-served` targets must not gain its listener, runtime database,
custody, KMS, or provisioner closure.  The offline provisioner is never linked or
installed with the online authority.

## Protocol and peer identity

- Expose only `POST /v1/management/status` over HTTP/1.1 on a dedicated TLS port.
  Serve exactly one request per connection and always respond with canonical JSON,
  an exact `Content-Length`, `Content-Type: application/json`, and
  `Connection: close`.  No redirect, upgrade, chunking, keep-alive, plaintext,
  proxy protocol, action route, or generic KB router is present.
- The request is one strict JSON object containing exactly four string fields:
  `nonce`, `target`, `target_mgmt_fp`, and `purpose`.  `nonce` is canonical
  unpadded base64url for exactly 32 bytes; `target` is a 1--127 byte ASCII token;
  `target_mgmt_fp` is exactly 64 lowercase hexadecimal bytes; and `purpose` is
  exactly `management.health.v1`.  Reject duplicate, missing, unknown, wrong-type,
  embedded-NUL, noncanonical, trailing, or oversized input.  Caller identity,
  generation, timestamps, key id, issuer, serial, and fingerprint are never
  accepted from JSON.
- Use a dedicated TLS context with `SSL_VERIFY_PEER |
  SSL_VERIFY_FAIL_IF_NO_PEER_CERT`, TLS 1.2 or newer, no renegotiation or
  compression, a configured server leaf/private key, and an explicitly configured
  management-client trust bundle.  Do not reuse the enrollment listener's
  intentionally optional-client-certificate context.
- Disable tickets, session-id caching, early data, renegotiation, compression, and
  post-handshake authentication; require security level 2 or stronger and negotiate
  only HTTP/1.1.  A resumed or cached TLS identity must never bypass fresh per-request
  primary authorization.
- Before reading HTTP, require a successfully chain-verified, time-valid leaf with
  only the intended client use, fixed CN `p5-kb-management`, and exactly one
  noncritical private marker OID `1.3.6.1.4.1.55555.5.1` whose exact value is
  `aimee-p5-kb-management-v1`.  Reject absent, wrong, critical, or duplicate marker
  extensions and wrong EKU/CN.  Extract normalized issuer, lowercase normalized
  serial, and SHA-256 of the complete DER leaf into an immutable verified-peer
  value; only that value may populate the lookup and signed caller fields.

## Strict transport

- Add a small authority-only, length-aware HTTP reader rather than linking the
  general KB TLS server.  Accept only the byte-exact request line
  `POST /v1/management/status HTTP/1.1`, with no absolute URI, alternate whitespace,
  or version.  Enforce a five-second absolute monotonic connection deadline,
  8 KiB headers, a 1 KiB body, at most 32 headers, exactly one nonempty Host,
  exactly one canonical decimal Content-Length, and the expected JSON media type.
  Reject transfer encoding, Expect, obs-fold, bare LF, control/NUL bytes, duplicate
  framing fields, integer overflow, surplus/pipelined bytes, and EOF-short bodies.
- Run a fixed-size worker pool with an explicit bounded accept queue and bounded
  per-peer/global in-flight work: four workers, a 32-socket queue, at most four
  queued/active connections per source address, and a bounded listen backlog.
  Overload closes the socket before TLS/request parsing and never consumes a worker.
  Signal shutdown stops acceptance, closes queued sockets, lets bounded active
  operations finish or cancel, rolls back guards, cleanses protected material, and
  joins every worker.  No detached threads or unbounded allocations are allowed.
- Map malformed framing/body to 400, an invalid management peer to handshake failure
  (or a generic 401 before any body is read), authoritative caller/target/fingerprint
  denial to 403, exact-use conflict to 409, and DB, custody, KMS/HWM, audit, seal,
  integrity, timeout, or dependency failure to a nondiagnostic 503.  Error bodies
  and logs disclose no identity, registry, seal, key, envelope, HWM, SQL, or provider
  detail.  All failure paths clear the output status and produce no stale signature.

## Online authority composition

- Make authority results typed end to end.  Fix the explicit runtime lookup adapter
  so PostgreSQL policy rejection SQLSTATE `28000` maps to DENIED while other SQL or
  connection failures remain unavailable.  Preserve the exact authoritative target
  fingerprint comparison before signing.  Exact replay/conflict is distinct from
  denial and failure.  `kb_mgmt_status_authority_issue` must zero its output before
  validation and on every unsuccessful exit.
- Each worker owns its own explicitly hardened libpq session opened through
  `db2_management_status_runtime_open`; no ambient `db2_conn` or shared libpq
  connection is used.  Add a narrow borrowed-connection initialization seam for
  the status key-use context so it cannot bypass the fixed login/role/search-path/
  row-security assertions.  Deployment may enable LOGIN on
  `aimee_kb_status_login` only after schema/grants have recreated it as NOLOGIN; it
  retains only membership in `aimee_kb_status` and the fixed definer EXECUTE set.
- On startup, open and harden every required DB session, hold the fixed startup
  snapshot, and require one enabled v2 registry whose custody id, wire id, public
  key, version, signed HWM attestation, and fresh provider HWM all agree with the
  configured KMS identity.  Initialize the durable seal epoch and KMS provider,
  verify the signed HWM under the pinned signer/domain, then commit the snapshot.
  Mismatch, rollback, malformed attestation, unavailable dependencies, or uncertain
  provider state prevents binding the socket.  A correctly bound but durably sealed
  authority may bind for diagnosable readiness, but every issuance remains generic
  503 until a freshly validated unsealed state is available; seal always wins.
- For each request, perform primary lookup from the verified peer, then invoke
  `kb_mgmt_status_custody_sign` using the DB-derived wire key id.  Custody retains
  the completed candidate/admit/guard protocol, signed-HWM check, WORM use-intent,
  seal checks, protected arena, and fail-closed cancellation behavior.  An admission
  committed before response loss stays one durable use and is never heuristically
  undone; retry requires a fresh nonce.
- Before any provider helper fork, remove unrelated credentials from its environment
  and close unrelated descriptors.  Construct a minimal explicit helper environment
  and reject loader/interpreter injection variables including `LD_PRELOAD`,
  `LD_LIBRARY_PATH`, `GCONV_PATH`, `LOCPATH`, and language-specific module paths.
  Protected plaintext mappings must be
  `MADV_DONTDUMP` and `MADV_WIPEONFORK` (or an equally fail-closed at-fork design),
  and helper diagnostics must not inherit authority secrets or emit provider output.

## Build and deployment isolation

- Introduce an explicit authority binary target with a hand-maintained object
  allowlist.  Do not reuse the current `STATUS_AUTHORITY_SRCS`, which includes both
  offline provisioner cores and omits the pure authority decision object.  Link only
  the dedicated listener/codec/main, pure status decision/wire code, custody and
  protected-use code, explicit status runtime/key-use DB adapters, required
  PostgreSQL/KMS/vault crypto objects, cJSON, libpq, OpenSSL, and pthreads.
- Add a dedicated runtime unit and minimal image/package.  It contains no
  provisioner, ordinary KB/server binary, shell, psql, curl, Python, CA private key,
  compiler, or generic HTTP client.  Run as a dedicated non-root identity with a
  read-only filesystem, private temporary/device namespaces, no capabilities or new
  privileges, core dumps disabled, bounded locked memory/files/tasks, syscall and
  namespace restrictions, and only the configured inbound status port plus outbound
  primary/KMS reachability.  TLS and KMS material is mounted read-only from
  root-owned paths; database credentials are provided separately and are not
  inherited by helpers.
- Plant `nm`/`readelf`/`ldd` and package-manifest gates proving the authority excludes
  provision/bootstrap symbols, ambient DB2 initialization, SQLite, enrollment,
  action/server routes, and generic clients; prove ordinary KB/server targets exclude
  authority listener, custody, status runtime, and provisioner symbols.

## Validation

- Unit/fuzz gates cover the exact JSON codec and HTTP framing state machine:
  missing/duplicate/unknown/wrong-type fields, canonical base64url, embedded NUL,
  trailing and oversized data, duplicate Host/Content-Length, transfer encoding,
  obs-fold, bare LF, header-count/number overflow, surplus bytes, deadlines, and
  fragmented reads.  ASAN/UBSAN/leak and cancellation tests prove output clearing,
  protected-memory cleanup, rollback, and joined workers.
- Pure/runtime tests prove TLS-derived caller identity is observed unchanged in
  lookup and output; target mismatch denies before signing; SQLSTATE 28000 is denial;
  other SQLSTATEs are unavailable; replay is conflict without a signature; every
  failure clears output; and a successful response round-trips through
  `kb_mgmt_status_from_json` and verifies under the provisioned public key.
- On CT260 run real PostgreSQL 17, the signed-HWM/KMS helper, the fixed provisioner,
  and the actual authority binary.  Use CT262 only as a remote TLS client/negative
  peer.  Seed one active target and one preissued management client because automatic
  enrollment belongs to B2.  Verify successful issuance plus exactly one use-intent
  and WORM record; no cert, foreign CA, expired/not-yet-valid, serverAuth-only,
  generic-clientAuth, wrong CN, and missing/wrong/critical/duplicate marker all fail.
  Exercise framing, oversize, slow-header, queue saturation, caller/target revoke
  races, primary/KMS/HWM/seal outages and rollback, clean restart, kill during slow
  headers, kill before admission commit, and kill after durable admission but before
  response delivery.  After every kill, restart must preserve binding, leave no
  stuck guard/lock, and issue successfully for a fresh nonce.
- Obtain the complete PVE container list before using a new VMID.  Touch only the
  integration CTs selected for this gate and drive them with pushed scripts followed
  by `pct exec <id> -- bash /root/<script>`.

## Deferred

P5-B2/B3 owns automatic per-instance management-client enrollment/renewal and the
complete server challenge -> KB -> authority -> signed health topology, including
revocation between challenge and action.  OIDC operator propagation and management
actions remain later P5 work; `/v1/management/action` is absent from this service.

## Delivery evidence

- Shipped the separately linked `aimee-kb-status-authority`, strict required-mTLS
  listener/peer profile, typed codec/runtime/custody composition, hardened systemd
  unit, and a scratch runtime image containing only the binary's dynamic-library
  closure. Static packaging, dependency, symbol, and forbidden-route gates pass.
- Focused normal and ASAN/UBSAN/leak tests pass for the runtime, key context,
  authority, custody, TLS peer, HTTP listener, and KMS helper. The KMS provider
  regression gate proves helper children cannot inherit an intentionally planted
  descriptor; the production helper path now closes stdin, stderr, and all fd>2.
- CT260 built and ran the actual binary against PostgreSQL 17 and signed-HWM KMS;
  CT262 proved the valid 13-field Ed25519 response, exact replay conflict,
  malformed/denied requests, no-cert and wrong-profile rejection, and absent-ALPN
  rejection. Durable intent/WORM counts and caller bindings agree.
- Startup PostgreSQL outage and HWM rollback refuse before bind; live HWM rollback
  returns 503 without a use; restoration and clean restart recover. A client RST
  after request dispatch leaves exactly one durable admission, and restart then
  signs a fresh nonce without a stuck guard.
