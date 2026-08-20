# P5-B3a live management-health exchange core

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** reviewed implementation plan.
- **Parent:** `tiered-llm-p5-oidc-control-plane.plan.md`, P5-B.
- **Depends on:** P5-A, P5-B foundation/B1a/B1b/B1c, P5-B2a/B2b/B2c.
- **Followed by:** P5-B3b production startup/route/auth composition and P5-B3c real-topology closeout.

## Boundary

Build the bounded, fail-closed orchestration core for the first live reverse-management
operation. The core composes a fresh primary registry snapshot, the B2c per-instance client
certificate, one pinned server TLS session, the dedicated online status authority, a second
fresh primary snapshot, and the server's existing nonce/HWM verifier. This packet compiles and
tests the real adapters but does not enable a production route or any management mutation.

The exchange is exactly:

1. Read snapshot A with one primary query inside a short tenant scope and release the scope
   before network I/O. Require exact `server_id`, safe endpoint, `status=active`,
   `enrollment_state=active`, empty `revoked_at`, canonical management issuer/serial/fingerprint,
   and `revocation_generation >= 1`.
2. Load the active B2c bundle and public metadata. Require a current, unrevoked active leaf.
3. Open one server TLS connection through the untrusted registry-endpoint connector. Present
   the B2c leaf and pin the enrolled server leaf by CA/hostname/EKU plus exact issuer, normalized
   serial, and DER fingerprint.
4. On that connection, `POST /v1/management/challenge`. Strictly decode only canonical
   `{nonce,expires_at}`, require a 32-byte base64url-no-pad nonce and expiry in `(now,now+15]`.
5. Send exact `{nonce,target,target_mgmt_fp,purpose}` to the dedicated authority using the same
   B2c leaf. The authority endpoint comes only from root/operator configuration and uses a
   separate strict TLS connector. Normal CA/hostname verification is followed by a mandatory
   constant-time DER-SHA256 leaf pin. Configuration holds one required pin and may hold one
   explicit secondary pin for a bounded rotation overlap. It cannot accept a registry endpoint
   or relax the registry connector's public-address policy.
6. Strictly decode the existing staple, then locally require the configured status key id,
   Ed25519 signature, lifetime, loaded B2c caller issuer/serial/fingerprint, target id and
   management fingerprint, purpose `management.health.v1`, and generation. Ordinary kb receives
   only the public verification key and never the status signing key.
7. Read snapshot B with a second primary query in a new short tenant scope immediately before
   the protected write. Require exact equality with A for `server_id`, endpoint, management
   issuer, serial and fingerprint; require both states still exactly `active`, `revoked_at` still
   empty, and `B.revocation_generation >= A.revocation_generation`. A legitimate generation
   increase does not invalidate the already-issued logical request, and the staple need not equal
   B if a later unrelated revocation advanced B.
8. On the original server TLS connection, send `GET /v1/management/health` with the bounded
   staple header. Never reconnect between challenge and health. Require 200 and strict bounded
   `{status:"ok",server_id:<exact target>}`.

## Public contract and adapters

Add `src/kb/kb_management_health_exchange.{h,c}`. Its result enum is exactly `OK`, `NOT_FOUND`,
`DENIED`, `CONFLICT`, `UNAVAILABLE`, `INTEGRITY`, and `INVALID`; B3b will map them without parsing
dependency strings. A dependency struct carries explicit context pointers and bounded callbacks
for snapshot load, active-bundle load/clear, server session open/request/close, authority issue,
wall-clock seconds, and monotonic milliseconds. Sessions are opaque handles. Tests assert two
snapshot calls, one authority call, one server open/close, and the identical session handle for
challenge and health. Every exit clears the bundle and all request/staple/response buffers.

Compile real adapters in B3a. Each snapshot adapter enters `db2_tenant_scope_begin()` using the
request actor, performs one primary snapshot query, then commits or rolls back before returning;
no PostgreSQL transaction or lease crosses network I/O. The lifecycle adapter uses
`kb_management_cert_load_active()` and the transport adapters consume its PEM bundle only long
enough to initialize their SSL contexts. The caller then cleanses its copy before network I/O;
OpenSSL-owned key objects are released with their contexts on every exit.

Add private, length-aware challenge/health codecs. Reject duplicate/unknown/missing fields,
embedded NUL, padding or noncanonical base64url/decimal, overflow, trailing bytes, and over-cap
responses. Validate target as the existing 1..127 token grammar and fingerprints as exactly 64
lowercase hex before encoding authority JSON.

## Bounded transports

Add `src/kb/kb_mgmt_status_client.{h,c}` for the narrow trusted-config authority transport. It
performs exactly one POST per required-mTLS connection, enforces SNI/hostname/CA plus mandatory
leaf pin, and uses one caller-supplied absolute monotonic deadline for connect, TLS handshake,
write, and read. It accepts only one HTTP/1.1 response with exact Content-Length and bounded JSON;
EOF framing, transfer encoding, interim/3xx, upgrade, trailers, surplus bytes, proxy variables,
and plaintext fallback are rejected. Authority 400/403/409/503 remain typed internally; 200 with
an invalid staple is integrity failure.

Harden `src/kb/kb_mgmt_client.{h,c}` with absolute monotonic deadlines. Sockets are nonblocking;
connect, `SSL_connect`, `SSL_read`, and `SSL_write` retry only through `poll()` to the deadline.
Preserve one DNS result set, direct connection to the exact permitted sockaddr, SNI/Host from the
enrolled hostname, certificate pinning, and one `SSL *` for both requests. The challenge response
must explicitly permit reuse; health may close. EOF, partial timeout, duplicate framing, surplus
pipelined bytes, or redirect is terminal.

DNS cannot be covered honestly by socket polling. The production adapter therefore uses one
process-bounded resolver worker with a queue capacity of one and copied inputs. The exchange waits
on a monotonic condition-variable deadline. A timed-out job remains owned by that single worker;
new jobs fail unavailable until it completes and cleans `addrinfo`, so timeout cannot create
unbounded threads, jobs, or memory. Tests inject a resolver for deterministic success, mixed
public/private filtering, timeout, late completion, saturation, and cleanup.

The exchange has one overall monotonic deadline and no retry/reconnect. It also reads wall time
for protocol timestamps; after challenge decode it caps the remaining monotonic budget to the
challenge expiry. Callers retry only as an entirely new challenge cycle.

## Tests and validation

Add focused deterministic exchange tests for exact ordering and same-session use. Cover each
A/B predicate, a legitimate generation increase, malformed/expired/future challenge, authority
denial/conflict/outage, bad key id/signature/lifetime/caller/target/fingerprint/purpose, server
redirect/framing/timeout/close, wrong health target, same-channel and cross-channel nonce replay,
staple reuse on a fresh TLS session, cleanup, and output zeroing. Add local TLS transport tests for
deadline, resolver, framing, redirect, client-certificate, authority pin, and two-pin rotation.
Fuzz challenge, health, and authority response codecs and run ASAN/UBSAN.

Run existing management endpoint, client, status codec, authority, listener, peer, runtime,
server nonce/HWM, B2b, and B2c lifecycle tests plus production server/kb/status-authority builds.
The B3a CT260 gate uses real PG17 snapshot adapters and real TLS peers. Full status-authority and
server processes, revoke-between-challenge-and-issuance, restart/HWM, DNS rebinding, KMS/seal
outage, generic server-auth seam, and the production health route remain B3b/B3c acceptance gates.

## Security invariants and non-goals

The ordinary kb never gains the status signing key. Caller identity at the authority comes only
from verified mTLS. The server challenge remains bound to its TLS channel exporter, peer leaf,
target, and purpose and is consumed on every identifiable health attempt. A higher generation is
durable before server 200. Primary/authority/network/storage ambiguity fails closed.

No production `/v1/servers/{id}/health` change, startup singleton, server generic-auth seam,
`/v1/management/action`, operator JWT/OIDC/JWKS/jti, mutation, bearer fallback, generic proxy,
pooling, enrollment redesign, new signing path, relaxed registry SSRF policy, TOFU, redirect,
HTTP/2, or P5-C/D work belongs in B3a.

## Review disposition

The plan roundtable aligned and reported no issues. Genuine recurring findings extracted from the
replay and independent review are incorporated above: mandatory authority leaf pinning with
bounded overlap, separate wall/monotonic clocks and a resource-bounded DNS deadline, explicit
local staple signature/binding verification, exact A/B predicates, typed failures, and concrete
adapter/test cardinality.
