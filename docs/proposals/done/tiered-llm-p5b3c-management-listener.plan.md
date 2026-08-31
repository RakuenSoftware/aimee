# P5-B3c distinct management listener and two-node closeout

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE. Implementation and adversarial branch review converged; production CT260→CT262 gate passed.
- **Parent:** `tiered-llm-p5-oidc-control-plane.plan.md`, P5-B.
- **Depends on:** P5-B3a health exchange and P5-B3b production wiring.
- **Followed by:** P5-C operator/OIDC propagation, then P5-D closeout.

## Boundary

Close the P5-B production boundary by giving the two existing server management routes their own
required-mTLS listener and proving the complete kb→server→status-authority flow on a real two-node
topology. This slice owns listener provenance, a separate TLS context and trust domain, strict
configuration/startup behavior, live cross-lane authorization tests, and topology validation.

It does not add a management action route, change the B3a status protocol, weaken registry SSRF
policy, add connection pooling, redesign enrollment, or claim OIDC propagation. The topology may
use CT260 and CT262 only; the Proxmox inventory must be read in full before either is changed.

## Listener provenance and authorization

Add an `is_management` bit to the HTTP connection job and carry it from accept through TLS setup
and `handle_conn`. Add a fourth poll slot and a separately bound management fd. Existing UDS,
plain TCP, and data TLS connections always carry a false management bit; only accepts from the new
fd carry true. The worker has one cleanup path: management TLS failure, short read, parse failure,
and normal completion all close the fd and unregister/free any SSL without calling `handle_conn`
after a failed handshake. An `accept` failure has no connection object to clean and stays in the
listener error path; once a fd exists, only the worker cleanup owns `SSL_free` and `close`. Request
line/header overflow is a parse failure: return a bounded 400 when possible, close, and never run
the classifier or dispatch. The management bind accepts a canonical numeric IPv4 address, never
DNS, and is not coupled to the data-plane bearer requirement. It rejects unspecified/wildcard,
broadcast, multicast, and link-local addresses. Precisely, accept 127.0.0.0/8 or any canonical
numeric IPv4 unicast outside `0.0.0.0`, `255.255.255.255`, `224.0.0.0/4`, and
`169.254.0.0/16`; binding an address not assigned to the host still fails at `bind`.

Extend the B3b three-way management classifier with listener provenance. It allows only exact
`POST /v1/management/challenge` and exact `GET /v1/management/health` when all of these hold:
management listener, successfully chain-verified leaf, management profile marker, and exact
compiled-in `p5-kb-management` CN. Any management route on a non-management listener, any generic route on the
management listener, or any management-profile leaf on a generic route is denied before generic
roster/bearer/bootstrap fallback. Ordinary data-plane identities and routes remain not-applicable
and retain current behavior. Management requests use zero generic capabilities, do not advance the
generic mTLS ramp, and do not consume the generic bearer rate bucket. The classifier runs in the
single `handle_conn` chokepoint immediately after bounded HTTP parsing and verified-peer
extraction, before every route-specific dispatch, roster, bearer, bootstrap, capability, SSE, or
upgrade path; malformed requests and `CONNECT` cannot bypass it. Both route handlers retain
their independent profile, channel, nonce, staple, issuer/serial/fingerprint, and HWM checks.
On the management lane the bounded parser also rejects unterminated/oversize headers, duplicate or
ambiguous Content-Length, Transfer-Encoding, Expect, and Upgrade before authorization; the B3
client's canonical zero-body challenge and header-only health requests remain accepted.

The kb's active B2c management leaf is a dedicated certificate and key with exact CN
`p5-kb-management`, exactly one established **non-critical** management marker extension, and
clientAuth-only EKU. (The marker is security-critical to the profile but deliberately non-critical
in X.509 so it matches the shipped B2c issuer/verifier contract.) It is distinct from
the CT262 management server's serverAuth-only leaf and from the status-authority TLS leaf. The
management TLS handshake verifies client purpose; the existing exact-profile extraction then
rejects missing/duplicate marker or CN and any extra EKU such as serverAuth before authorization.
Extend `server_tls_peer_cert` itself to parse `NID_ext_key_usage` and set `management_profile` only
when the EKU sequence contains exactly `id-kp-clientAuth`; table tests cover clientAuth-only,
serverAuth-only, dual-EKU, missing EKU, missing CN, and the exact valid leaf.

## Separate TLS trust domain

Add a process-owned management `SSL_CTX` in `server_tls`, with a context-selecting accept helper and
`server_tls_management_begin`. The management context always requires a client certificate,
enforces TLS 1.2 or newer and client-auth chain/time/purpose verification against its dedicated CA,
and uses the configured management server leaf/key. A separate management context builder has no
generic DB1 roster callback parameter or registration call by construction: management leaves are
primary-issued and their online next-request authority is the B3 nonce/staple protocol. A focused
test marks the same leaf revoked in the generic roster and proves this TLS context still performs
only CA/time/EKU verification while application status revocation remains fail-closed. The data
context and optional→required ramp are unchanged.

The context is published only after it is completely built and remains process-lifetime owned
because detached HTTP workers are not joined by current server shutdown. Once published it is
never freed or replaced before process exit; each `SSL_new` also holds its OpenSSL reference.
Start failure before publication frees the private candidate. Same-process HTTP restart may reuse
only a byte-identical cert/key/CA configuration packet; any difference is a fatal start while the
published context remains untouched. Live `SSL` objects share the existing
registered-connection shutdown. Management TLS reload is explicitly restart-required; data
SIGHUP never replaces or frees the management context.

“Byte-identical” is a stored SHA-256 tuple over the bounded raw PEM bytes in fixed
`certificate || private-key || client-CA` order, read through no-follow regular-file descriptors
at context build time. On a later start, all three hashes must match before reuse; mismatch returns
`SERVER_HTTP_START_MGMT_FATAL`, never swaps the context, and never rebinds a listener. Bind/port
are separately revalidated but are not part of the TLS-context tuple.
The same single captured PEM buffers, not reopened pathnames. Build the candidate certificate chain,
private key, and CA store, closing the hash/load TOCTOU window; the buffers are cleansed after the
private candidate is built.

## Configuration and startup

Use one all-or-none operator environment packet:

- `AIMEE_SERVER_MGMT_PORT` (canonical decimal 1..65535);
- optional `AIMEE_SERVER_MGMT_BIND`, default `127.0.0.1`, canonical numeric IPv4 only;
- `AIMEE_SERVER_MGMT_TLS_CERT`, `AIMEE_SERVER_MGMT_TLS_KEY`, and
  `AIMEE_SERVER_MGMT_CLIENT_CA` (nonempty absolute bounded paths).

All core fields absent means disabled. Empty or partial fields, malformed port/bind/path, invalid
CA/certificate/key, key mismatch, or bind failure is configured-invalid and fails server startup.
When configured, preflight the handler packet too: canonical `AIMEE_SERVER_ID`, token
`AIMEE_MGMT_STATUS_KEY_ID`, and exact 64-lowercase-hex `AIMEE_MGMT_STATUS_PUBLIC_KEY`. The named
`SERVER_HTTP_START_MGMT_FATAL` result propagates through `server_main`, whose redacted diagnostic
identifies the invalid variable or startup stage; optional legacy listener failure
semantics stay unchanged. Every error path and `server_http_stop` closes the management listen fd.
No secret is logged.

## Tests

Focused tests cover:

1. the full classifier cross-product of listener, exact route, method, verified chain, profile,
   and CN, including denial of generic routes on the management lane and unchanged data behavior;
2. disabled, partial, empty, malformed, boundary, and valid configuration packets, including
   handler inputs, absolute paths, numeric bind, wildcard/link-local/multicast rejection, occupied
   port, and fatal startup propagation;
3. real management TLS handshakes for the correct CA/profile, missing client cert, unknown CA,
   wrong EKU, server key mismatch, generic-roster callback isolation, and peer/local certificate
   extraction through `server_tls_peer_cert`/`server_tls_local_fingerprint`;
4. live loopback HTTP proving bearer-free management success only on the management listener,
   cross-lane denial, generic route denial on that listener, and unchanged generic ramp/rate state;
   when management is disabled the poll set contains only the existing slots rather than a zero-fd
   placeholder;
5. existing server HTTP/TLS/management status, B3a transport/exchange, kb runtime/route, and
   production server/kb/status-authority builds; plus fresh ASAN/UBSAN focused gates.

## Real topology gate

Use CT262 for the server and CT260 for kb/status-authority dependencies. Because the registered
endpoint connector correctly rejects RFC1918 and RFC2544 benchmark space, add isolated TEST-NET-1
aliases `192.0.2.1/30` on CT260 and `192.0.2.2/30` on CT262 and register a stable hostname that
resolves only to the CT262 alias. This documentation-only address space is isolated inside the two
throwaway guests and is currently accepted by the untrusted connector without a code change. A
gate proves all three TEST-NET ranges are accepted by the existing predicate while RFC1918,
RFC2544, loopback, link-local, multicast, unspecified, and IPv6 ULA remain rejected. Only
192.0.2.0/30 is assigned, only inside CT260/CT262, and both aliases and host entries are removed at
gate teardown. Do not weaken `kb_mgmt_endpoint` or touch any other guest.

The primary management CA and CT262 server leaf are generated in a bounded gate workspace on
CT260. The CA bundle and server leaf/key are copied out-of-band with `pct push` to explicit
root-owned mode-0600 paths under `/root/p5b3c/` on CT262. SHA-256 digests are recorded before
transfer and checked on CT262; `openssl verify` must prove the server leaf and kb client leaf chain
to the same dedicated management CA with no data-plane CA or cross-sign in the bundle, and a
one-byte-tampered bundle must fail initialization. The gate records and rechecks separate hashes
for the CA, CT262 leaf/key, and CT260 kb leaf/key; it records the CT262 leaf issuer and serial too.

The exact-commit gate builds clean archives and uses production binaries/configuration. It proves:

- happy-path challenge→authority→health over distinct management mTLS;
- correct listener/certificate/CA/profile/CN matrix and inability of the management leaf to use a
  generic route;
- server restart with durable nonce/HWM continuity and rejection of replay/rollback;
- caller or target registry revoke/mutate between the two B3a snapshots;
- revoke between challenge and status issuance using a gate-only CT260 loopback proxy: it accepts
  exactly one connection in a gate window from the expected local kb source to the fixed authority
  destination/SNI, buffers at most the authority protocol's production request cap before the
  hold, signals a root-only control fifo, waits at most five seconds for a release token containing
  the window nonce after the PG revocation commits, then forwards to the unchanged authority; a
  second connection, size overflow, wrong correlation, timeout, or control loss hard-closes both
  sides and must yield `UNAVAILABLE`, never a staple. One single-threaded proxy process owns the
  socket, fifo, and monotonic timer in one `poll` loop; timer/error readiness wins over release,
  and tests assert the authority observes zero bytes for every failure case;
- authority, PG, workload helper, KMS/seal, and target outage fail-closed behavior;
- DNS rebinding/mixed-address, redirect, wrong target identity, and wrong leaf rejection.

Where B3a opens one connection per request, next-request revocation is additionally proven on a
raw same-TLS-session harness; the production topology report must not claim pooling.

The management listener registers no route of its own: lane authorization permits only the two
existing exact route-table entries. Tests enumerate the route table and prove every other path,
including `/v1/management/action`, generic health, `CONNECT`, and upgrade attempts, is denied on
that lane. Adding a management route requires a new reviewed boundary.

## Merge gates

Plan roundtable with full context, delegated implementation, local production/focused/sanitizer
validation, exact-commit CT260/CT262 topology, adversarial full-branch roundtable and convergence,
all CI, PR merge to `testing`, delivery-table update, and an aimee memory are mandatory. Any
ambiguous listener, authentication, configuration, or topology state fails closed.
