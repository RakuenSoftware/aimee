# Proposal: Faster mTLS transport between thin clients, aimee-server, and aimee-kb

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

> **Archived delivered scope (2026-07-26).** This proposal is retained as the historical
> specification for work already delivered. Remaining work is tracked in
> [`mtls-transport-rollout-evidence.md`](../pending/mtls-transport-rollout-evidence.md).

- **State:** DONE — delivered scope archived 2026-07-26.
- **Date:** 2026-07-22
- **Implementation updated:** 2026-07-22
- **Scope:** thin-client ↔ aimee-server and aimee-server ↔ aimee-kb transport only
- **Related:** `tiered-llm-p8-thinclient-mtls.md`,
  `tiered-llm-p8a-mtls-per-request-revocation.plan.md`, and
  `v1-stability-and-distributed-validation.md`
- **Initial design roundtable:** `oprun_g6a60868f92dbc01_1784712069_9`

## Thesis

The baseline implementation spent more transport time establishing connections
than moving useful bytes. Both remote paths used one-shot HTTP/1.1;
server→kb additionally rebuilt its TLS context, resolved the peer, connected,
completed an mTLS handshake, served one request, and closed. The kb mTLS listener
also handled accepted connections serially.

The first performance packet is therefore **bounded concurrency plus connection
reuse**, not a protocol rewrite. HTTP body compression is the next explicit lever:
it ships for sufficiently large buffered thin-client JSON/text where reduced wire
time pays for its CPU cost, and remains benchmark-gated on the normally faster
server→kb link. HTTP/2 stays deferred until pooled HTTP/1.1 has been measured and a
remaining multiplexing bottleneck is proved. Aimee will not hand-roll HTTP/2.

This ordering preserves automatic mTLS and the existing per-request certificate
revocation model. A reused authenticated connection is never permission to skip
request-layer identity, revocation, or capability checks.

## Decisions

| Question | Decision |
|---|---|
| What ships first? | Concurrent kb accept/serve, strict reusable HTTP/1.1 framing, then a bounded server→kb connection pool. |
| Is body compression part of the design? | **Yes.** Negotiated HTTP content encoding is a first-class packet with per-link defaults and promotion gates below. |
| Thin-client compression default | Buffered JSON/text bodies ≥4 KiB: gzip on when both peers negotiate it, except sensitive or streaming routes. Smaller bodies stay uncompressed. |
| Server→kb compression default | Off initially. Promote buffered JSON/text ≥16 KiB to gzip-on only when the remote-link benchmark proves ≥10% end-to-end latency improvement or ≥35% wire-byte reduction without violating CPU/tail budgets. |
| zstd | Experimental later for controlled server↔kb peers. gzip is the compatibility baseline; zstd never becomes implicit without codec negotiation and cross-build coverage. |
| HTTP/2 | Deferred. Eligible only after pooled HTTP/1.1 passes and still exhibits the multiplexing gate in §P5. Use a mature library; never implement framing or HPACK in-tree. |
| TLS 0-RTT | Disabled. Replay-sensitive mutation semantics do not justify it. |
| Automatic retries | Reads only, before any response byte, on a bounded transport-failure allowlist. Mutations are not automatically retried without a separately specified idempotency contract. |
| Short-lived CLI processes | Keep-alive helps multiple calls within one process, streams, webchat, gateways, and other resident clients. It cannot reuse a socket across separate hook/CLI processes. Cross-process brokering is not smuggled into this packet. |

## Implementation record

The engineering packets are merged to `testing`. All behavior-changing transport
features remain off by default until the P0/P7 measurement and canary gates are
satisfied; implementation completion is not evidence for default promotion.

| Packet | Merged delivery | Result |
|---|---|---|
| P0 | PR #1782 (`18f67e32`) | Reproducible transport artifacts, impairment profiles, and `latency_slo.v1` evaluation contract. |
| P1 authority | PR #1786 (`2a551642`) | Immutable certificate issuer/serial authority checks fail closed. |
| P1 framing | PR #1787 (`818a5b4c`) | Strict bounded reusable HTTP/1.1 framing. |
| P1 kb reuse | PR #1795 (`9c424004`) | Bounded concurrent kb TLS serving and persistent request loop. |
| P2 client framing | PR #1799 (`297591cb`) | Exact response framing for server→kb reuse. |
| P2a pool | PR #1800 (`16c769e6`) | Bounded server→kb TLS connection pooling. |
| P4 | PR #1803 (`447705ef`) | Long-lived TLS contexts and bounded session reuse. |
| P7 mechanism | PR #1805 (`9cd7a115`) | Independent, live-reloadable, default-off rollout flags. |
| P3 | PR #1811 (`a3cb11d3`) | Negotiated bounded gzip for allowlisted buffered thin-client routes, with codec-less fallback. |
| P2b | PR #1813 (`df23f9e7`) | Bounded per-thread thin-client TLS reuse and reusable server loop; streaming stays one-shot. |

The remaining work is operational evidence, not another unconditional transport
implementation packet:

- run the declared ≥10,000-attempt profiles and retain their artifacts before
  promoting any default;
- progress P7 cohorts only when their 24-hour gates pass, with an unenabled
  control cohort and automatic rollback active;
- leave server→kb gzip off unless its separate remote-link threshold is met;
- leave HTTP/2 deferred unless the P5 multiplexing gate is reproduced; and
- leave P6 closed unless measurements identify repeated, safe sequential calls.

No production result, 24-hour soak, or 10,000-attempt artifact is claimed by this
record. Until those exist, one-shot uncompressed HTTP/1.1 remains the default and
is the rollback path.

## §0 Baseline and measured hypothesis

The proposal was grounded in the pre-implementation code, not a generic HTTP wish
list. The following bullets describe that baseline and are retained for audit:

- `src/aimee_client.c` opens a socket, completes TLS, sends
  `Connection: close`, reads until EOF, and tears the connection down for each
  remote request.
- `src/server/server_tls.c` deliberately advertises only `http/1.1` over ALPN
  because the hand-written server cannot speak h2.
- `src/server/server_http.c::handle_conn` parses and dispatches one HTTP/1.1
  request. The TLS listener already offloads accepted connections to bounded
  workers (`CONN_LIVE_MAX=64`), but each worker serves only one request.
- `src/modules/kb_client/kb_client_mtls.c` snapshots the enrolled identity and
  calls the kb TLS client for every remote kb operation.
- `src/kb/http/kb_tls.c::kb_tls_client_request_auth` builds a fresh `SSL_CTX`,
  resolves DNS, opens TCP, completes TLS, sends `Connection: close`, reads to EOF,
  and destroys the context for every call.
- `src/kb/http/kb_tls_serve.c` accepts a connection and calls
  `kb_tls_serve_conn` synchronously on the listener thread. It parses one request,
  checks the verified peer fingerprint through the legacy enrollment revocation
  helper, returns
  `Connection: close`, and closes.
- The kb framing limits at baseline were 64 KiB for a whole request and 256 KiB for the
  route response; the server-side kb client allocates a 1 MiB response buffer.
- TLS contexts use a TLS 1.2 minimum and can negotiate TLS 1.3 where the platform
  supports it. TLS-level compression is not used and must remain disabled.

The expected ranking is:

1. Remove serialized kb serving and repeated server→kb handshakes.
2. Compress large eligible bodies when wire savings exceed codec cost.
3. Reduce avoidable application round trips.
4. Adopt HTTP/2 only if concurrency measurements still prove HTTP/1.1 pool
   head-of-line blocking.

P0 measurements, rather than that expectation, decide promotion.

## §1 Invariants

Every packet preserves these conditions:

1. Both network links remain TLS-protected and use mTLS wherever their related
   mTLS rollout requires it.
2. The verified certificate identity is captured from the TLS object, never from
   a caller-supplied forwarding header.
3. Certificate validity, expiry, durable revocation, request authentication, and
   route capability are checked on **every HTTP request**, including requests on a
   reused connection. Handshake success alone never authorizes later requests.
4. A cert revoked while a connection is alive is refused on that connection's
   next request. The connection is then drained/closed and all idle pool entries
   for the same `(issuer, serial)` are evicted.
5. Compression changes bytes on the wire only. Decompressed bytes presented to
   handlers remain API-equivalent to the uncompressed request/response.
6. No TLS compression, no 0-RTT mutation replay, no spoofable proxy identity, and
   no hand-written HTTP/2.
7. Resource use is bounded: connections, workers, queued borrowers, request
   framing, decompressed bytes, compression ratio, timeouts, and retries.
8. Old `Connection: close` clients continue to work throughout rollout. A peer's
   explicit close request is always honored.

## P0 — Transport benchmark and observability contract

Before changing defaults, add a transport benchmark covering both links. Each
sample records:

- DNS, TCP connect, TLS/mTLS handshake, pool wait, request write, request auth and
  revocation check, handler queue, handler, compression/decompression CPU and wall
  time, response serialization, time-to-first-byte, and final byte;
- raw and wire bytes by direction, selected content encoding, connection age,
  request count on the connection, reuse/resumption state, and close/eviction
  reason;
- open/idle/busy/draining pool entries, kb accept backlog, active handshakes,
  errors, retries, CPU, RSS, and file descriptors.

P0 implemented the `latency_slo.v1` evaluator specified in
`docs/BENCHMARKS.md` and bound it to `make check-latency-slo`; no later promotion
gate may claim conformance before that target passes on the candidate artifact.
Its contract is at least 10,000 eligible attempts, fixed
eligibility before execution, nearest-rank percentiles, and the one-sided 95%
confidence bound for the combined failure/tail budget. The applicable regression
ceilings are the path budgets in `docs/BENCHMARKS.md` (global <10 ms p50 / <20 ms
p99, with stricter named paths winning).
Profiles are explicit:

- loopback/control;
- LAN server→kb: 1 ms RTT, no intentional loss, ≥1 Gbit/s;
- constrained remote server→kb: 30 ms RTT, 100 Mbit/s, 0.1% loss;
- WAN thin-client: 80 ms RTT, 20 Mbit/s down / 5 Mbit/s up, 0.5% loss;
- cold connection, warm sequential, 8/32/64 concurrent requests, large buffered
  JSON, incompressible JSON, and streaming SSE/NDJSON.

Baseline and treatment run on the same build host and network profile. A packet
does not promote if error rate increases, the existing path SLO fails, or its
claimed improvement falls inside the measurement confidence interval.

## P1 — Concurrent, framed kb mTLS serving

Decouple `accept()` from `kb_tls_serve_conn` using the same bounded-worker shape as
the server listener. The initial cap is 64 live kb TLS connections, aligned with
the server's existing `CONN_LIVE_MAX`; it is configurable downward. Hitting the
cap closes the new connection promptly. Once HTTP parsing is available, overload
returns `503`, `Retry-After: 1`, and `Connection: close`.

Make the HTTP/1.1 parser reusable and strict before enabling keep-alive:

- maximum request line 8 KiB, URI 4 KiB, 64 headers, and 64 KiB total request
  head+body, preserving the existing request allocation ceiling;
- header read deadline 10 s, request/response I/O deadline 30 s, and idle
  keep-alive deadline 30 s;
- `Content-Length` framing is required for bodies in the first packet; ambiguous
  duplicate lengths, simultaneous chunked+length framing, obs-fold, NUL, bare CR,
  or incomplete bodies fail closed with `400` and connection close;
- no request pipelining. HTTP/1.1 reuse remains strictly one in-flight request per
  connection;
- response length is parsed and read exactly; EOF is no longer the success
  delimiter on a reusable connection. Response heads are capped at 64 KiB and
  transport response bodies at 1 MiB, while an existing route's smaller 256 KiB
  limit remains in force. P3 applies the same 1 MiB ceiling to decompressed bytes
  plus its ratio bound; compressed wire bytes may never bypass either limit.

The enrollment revocation lookup remains inside the per-request loop. P1 adds
verified-peer issuer and normalized-serial extraction to `src/kb/http/kb_tls.c`
(alongside the existing CN/fingerprint helpers) and threads the immutable tuple
through `kb_tls_serve_conn` and request context; no CN-only or fingerprint-only
fallback is accepted for pooled authorization or eviction. Before
reuse is enabled, replace the legacy fingerprint-only boolean check with the
immutable `(issuer, normalized serial)` primary-authoritative tri-state required
by the tenancy/identity proposal: active, revoked, or authority-error. Revoked and
authority-error both refuse the request. This closes the current helper's
documented fail-open-on-DB-outage behavior rather than making that gap live longer
on a pooled connection. This packet does not introduce network OCSP/CRL fetching
on the request hot path: kb owns the enrollment state already, and the server owns
its thin-client PKI state.

The tri-state check runs at the top of every iteration of the reusable request
loop, after syntactically parsing the complete request head but **before route
resolution**, body decompression, or any route, bootstrap, streaming, websocket,
mirror, or other early-return dispatch. Parsing may retain the request-target
bytes but performs no route lookup and emits no route-dependent response before
the authority result. An authority error returns `503` and closes; revoked,
expired, or unrecognized identity returns `403` and closes. A live-connection test
revokes after request N and proves request N+1 is refused before its first response
body byte; a DB-unavailable test proves no route resolution or dispatch occurs.

P1 acceptance:

- 64 slow handshakes cannot block the accept loop or create an unbounded number of
  workers;
- malformed framing, slowloris, early EOF, oversized body, and timeout tests close
  only the offending connection;
- missing issuer/serial extraction, revocation authority error, and DB outage
  fail closed before dispatch;
- response head/body overflow and, once P3 lands, ratio-bomb tests abort and close
  without exceeding the 1 MiB client allocation;
- concurrent clients make forward progress, with no global serialization in the
  listener;
- ASAN/UBSAN and file-descriptor leak tests pass across 100,000 connections.

P1 can merge after its correctness tests, but it cannot be enabled beyond the CI
cohort or claim performance acceptance until P0 has landed and
`make check-latency-slo` passes on the same build and declared profile.

## P2 — Persistent HTTP/1.1 and bounded server→kb pooling

The first production pool is server→kb because both endpoints are resident
processes and the server makes repeated kb calls. It is keyed by
`(resolved endpoint, server-name, client certificate issuer+serial, trust-store
generation)` so identities or trust generations never share a socket.

Initial per-endpoint bounds are:

- 2 idle connections, 8 total connections;
- 30 s idle timeout, 10 min maximum connection age, 1,000 requests per
  connection;
- one in-flight request per HTTP/1.1 connection;
- at most 64 waiting borrowers process-wide, each bounded by the caller's
  deadline; exhaustion returns non-retryable
  `KB_CLIENT_ERR_POOL_EXHAUSTED` to `kb_client_mtls.c`, increments
  `kb_pool_borrow_exhausted_total{endpoint_id}`, and fails rather than growing the
  pool. The bounded `endpoint_id` label contains no certificate identity. An HTTP
  caller maps this condition to `503` without an automatic transport retry;
- one concurrent replacement handshake per pool key, preventing a revocation or
  outage from creating a reconnect storm.

`SSL_CTX`, parsed CA/certificate/key material, and resolver state become
long-lived objects owned by the kb transport manager. The context is immutable
after publication and reference-counted across in-flight handshakes. DNS respects
TTL and refreshes without moving an established verified connection to a new
address. Certificate/trust reload creates a new generation; old entries become
draining and receive no new requests.

### Per-request identity and revocation

The chosen first-delivery mechanism is the existing application-layer tuple, not
TLS post-handshake authentication:

1. At handshake, capture the verified peer `(issuer, serial, CN/profile)` from
   `SSL` and attach it immutably to the connection object.
2. Before every request dispatch, re-check that tuple's expiry, enrollment, and
   durable revocation status and rebuild request context/capabilities.
3. On invalid/revoked/unrecognized status, reject that request, mark the
   connection non-reusable, and evict matching idle entries. Eviction is serialized
   per identity and does not open replacement connections for a revoked identity.

The transport manager maintains a process-wide secondary index from
`(issuer, normalized serial)` to every connection and pool key. A revocation
observed on any endpoint publishes one identity-generation change and atomically
marks all matching busy entries draining and closes all matching idle entries,
including entries for other resolved endpoints or replicas. No new borrow for
that identity succeeds while eviction is in progress.

An authenticated `503 revocation-authority-unavailable` response places that pool
key in a no-replacement cooldown: queued borrows fail, the close does not trigger
an immediate handshake, and one bounded health probe retries after exponential
backoff from 1 s to 30 s. Success clears the cooldown. This prevents an authority
outage from turning fail-closed request checks into a reconnect storm.

This is already the server-side P8a model. The kb reusable loop upgrades the
current `db2_enrollment_is_revoked` boolean/fingerprint seam to the immutable-key,
tri-state fail-closed contract above and must never move it to handshake-only
code.

P2b applies the same contract to thin-client→server keep-alive. Before any
reusable server connection is enabled, every request on it must re-check the
immutable verified-peer `(issuer, normalized serial)` through the server-owned
primary authority before decompression or dispatch. Revoked and unknown identities
fail `403`; authority failure fails `503`; either result closes the connection.
P2b acceptance includes live-revocation and authority-unavailable tests proving
that no route, bootstrap, streaming, websocket, or early-return path dispatches.

### Drain and retry semantics

On certificate rotation, trust/config reload, revocation generation change, max
age, or shutdown:

- mark the affected entry draining atomically;
- never assign another request to it;
- allow its current response to finish within the original request deadline;
- close it immediately after that response, or force-close at the request
  deadline; there is no separate unbounded drain ceiling;
- invalidate associated cached TLS sessions when identity/trust changes.

Automatic retry is deliberately narrow:

- only GET/HEAD routes listed in a reviewed `transport_retry_safe` table as
  side-effect-free; the verb alone is not sufficient and new routes default off;
- only once;
- only when no response byte has been received **and zero application request
  bytes were reported written**. A connect failure or stale-socket failure on the
  first write is eligible only when that write reports zero; once any request byte
  may have entered the transport, dispatch is ambiguous and no automatic retry is
  issued, regardless of route or verb;
- only for connect failure, zero-byte stale-socket first write, `ECONNRESET`,
  `EPIPE`, or retryable TLS close satisfying that zero-request-byte boundary;
- every retry uses a new/revalidated connection and repeats identity/revocation
  checks;
- POST/PUT/PATCH/DELETE and streaming requests are never automatically retried in
  this packet, even when a route is believed to be logically idempotent.

P2 acceptance:

- a 1,000-request warm sequential server→kb test performs one TCP/mTLS handshake,
  subject to configured age/rotation limits;
- revoking the peer after request N makes request N+1 fail on the same live
  connection and evicts matching idle entries;
- rotating the cert while requests are active completes bounded in-flight work
  and sends every subsequent request with the new identity;
- pool saturation, kb restart, half-close, stale idle socket, DNS change, and
  partial response tests neither duplicate mutations nor leak descriptors;
- warm p50 TTFB is ≤0.70× baseline and p99 TTFB is ≤0.90× baseline on at least
  one declared remote profile, with no existing SLO/error regression.

## P3 — Negotiated HTTP body compression

Compression is application-level HTTP content encoding, not TLS compression and
not the unrelated LLM-context economizer.

### Default matrix

| Direction/profile | Buffered JSON/text | Streaming SSE/NDJSON | Sensitive routes |
|---|---|---|---|
| thin-client→server request | gzip when negotiated and raw body ≥4 KiB | off | off |
| server→thin-client response | gzip when negotiated and raw body ≥4 KiB | off | off |
| server→kb request | off initially; measurement-gated gzip at ≥16 KiB | off | off |
| kb→server response | off initially; measurement-gated gzip at ≥16 KiB | off | off |

Sensitive routes include enrollment, certificate/key/token operations, vault
operations, authorization/bootstrap responses, and any response that mixes a
secret with caller-controlled reflection. The implementation uses an explicit
route allowlist, not a fragile denylist: adding a route does not make it
compressible automatically.

The server/kb capabilities response advertises separate
`response_content_encodings` and `request_content_encodings` arrays. Absence of a
token means unsupported. A requester sends `Accept-Encoding: gzip` to negotiate
responses, which is safe for an old peer to ignore; an absent `Accept-Encoding`
means no response compression is accepted. It sends a compressed request only
after the peer has advertised gzip request decoding; otherwise it sends the
original body. A compressed sender sets `Content-Encoding: gzip` and the
compressed `Content-Length`. The complete header block, including authorization
and route-bound headers, is parsed first; authentication, verified peer identity,
route selection, and compressed-byte limits are then checked before
decompression. Decompression occurs before JSON parsing/handler dispatch.
Responses are authorized and fully serialized before compression. Capability
caches are scoped to endpoint plus server certificate identity and are invalidated
on reconnect, cert rotation, version change, or an unsupported-encoding response.

Safety bounds preserve today's transport envelopes:

- maximum decompressed request head plus body: 64 KiB. The permitted
  decompressed body is therefore `64 KiB - parsed_head_bytes`, never a separate
  64 KiB allowance;
- maximum decompressed response: 1 MiB;
- maximum expansion ratio: 50:1;
- enforce both incrementally at the decoder boundary before forwarding bytes;
- overflow, malformed stream, trailing data, or length mismatch aborts and closes
  the connection; decoder state is discarded, never reused;
- streaming stays uncompressed until a separate packet specifies flush/framing
  semantics and proves latency benefit.

Streaming compression is explicitly deferred to follow-up packet
`mtls-transport-performance-p3s`: it must define independent gzip streams,
flush/chunk boundaries, abort semantics, TTFT/inter-token gates, and trailer rules.
Until P3s is separately approved, streaming compression is a non-goal rather than
an implied future default. The `off` streaming cells above are authoritative for
this packet only; P3s is a successor packet and must independently pass the P0
measurement gates and the P7 cohort, soak, automatic-disable, and rollback process
before changing any cell.

gzip promotion requires, per eligible workload and link profile:

- median wire bytes ≤0.65× uncompressed; and
- end-to-end p50 or p99 latency improves by at least 10%; and
- combined codec CPU does not raise host CPU by more than 10% at the declared
  concurrency; and
- no SLO, error-rate, RSS, or descriptor regression.

Thin-client gzip is the intended default because remote thin-client links are
commonly bandwidth/RTT constrained. It still rolls out behind a capability flag
until the gate passes on Linux, macOS, and the supported TLS-terminating Windows
topology. Server→kb remains off where a fast LAN makes codec cost larger than wire
savings. Operators may enable the measured remote profile independently.

zstd is a later negotiated option for controlled server↔kb deployments. It needs
the same gates, a distinct encoding token, decompression bounds, and a runtime
codec probe on both peers; a build that cannot initialize the codec advertises no
zstd capability and never enters its decoder. It does not replace gzip on general
thin clients.

## P4 — TLS reconnect cost and short-lived thin clients

Prefer TLS 1.3 while retaining the existing TLS 1.2 compatibility floor. Reusing
the process-scoped `SSL_CTX` and TLS session cache improves reconnects inside
resident clients. Session tickets have bounded lifetime and are invalidated on
certificate/trust rotation and revocation changes.

The normal `aimee` hook/CLI binary is a short-lived process. A socket pool or
in-memory TLS session cache cannot survive from one invocation to the next. This
proposal therefore makes no false promise that HTTP keep-alive removes every
thin-client handshake. The options are:

1. accept the per-invocation handshake and make large bodies cheaper with P3;
2. reuse a connection within naturally resident clients such as webchat,
   gateways, streaming commands, and multi-call commands; or
3. separately propose a least-privilege resident client transport broker.

Persisting TLS sessions or adding a broker changes credential custody and process
authority, so it is not an incidental optimization here. TLS 0-RTT remains off.

## P5 — Falsifiable HTTP/2 gate

HTTP/2 is not part of P1–P4. It becomes proposal-eligible only after tuned pooled
HTTP/1.1 meets correctness gates and either condition is reproduced:

- at 32 concurrent independent requests to one peer, p99 TTFB is >2× the
  single-request p99 because all eight pool connections are occupied by slow
  responses; or
- the file-descriptor/connection cap required to meet the existing latency SLO is
  operationally unacceptable and multiplexing demonstrably meets it with fewer
  connections.

If eligible, implementation uses a maintained HTTP/2 library with bounded header
table, frame, stream, and connection memory. ALPN offers `h2,http/1.1`; old peers
fall back to h1. The verified mTLS peer remains connection-bound, while
revocation/auth/capability checks execute separately for every stream. Revocation
prevents new streams immediately and cancels/drains existing streams according to
the same request deadline. Reverse-proxy TLS termination is not accepted merely
by trusting an identity header; it requires a separately reviewed authenticated
handoff design.

## P6 — Round-trip reduction

After transport measurement identifies repeated sequential kb calls, add bounded
batch endpoints only for operations that are independently authorized,
order-independent, and safe to combine, such as multi-document fetch or
multi-vector query. Each item has its own result and size bound. Revocation and
capability checks occur before the batch and remain valid for every item; no batch
may include enrollment, revocation, capability minting, vault/secrets, streaming,
or mutations without a future idempotency contract.

P6 is optional. It must not delay P1/P2 and must prove that eliminated round trips,
not altered query semantics, caused the improvement.

## P7 — Canary rollout and automatic rollback

P2 and P3 ship behind independent, live-reloadable settings. After the
2026-07-22 three-node validation, the two connection-reuse settings default on;
the compression settings remain off:

- `transport.kb_pool_enabled=true`;
- `transport.server_keepalive_enabled=true`;
- `transport.thinclient_gzip_enabled=false`;
- `transport.kb_gzip_enabled=false`.

Each feature advances through: CI/impairment harness → one explicitly selected
server↔kb pair → 10% of enrolled server identities → 50% → 100%. Thin-client gzip
uses the same sequence by enrolled client identity and platform, beginning with
Linux, then macOS, then the documented Windows TLS-terminating topology. Each
stage soaks at least 24 hours and retains a comparable unenabled control cohort.

Promotion requires its packet's acceptance gates and the existing
`latency_slo.v1` path budget. Any authentication/revocation bypass, duplicate
dispatch, decompression-limit violation, process crash, descriptor/RSS growth, or
combined failure/tail confidence bound above 1% stops promotion and automatically
turns off the affected feature for that cohort. Compression also disables when
host CPU rises >10% or latency loses its required improvement. Pooling disables
when saturation/transport errors exceed the control cohort by 0.5 percentage
points, measured over at least 1,000 eligible attempts per cohort in the 24-hour
window. Below that floor, promotion remains paused and rollback uses the same
one-sided 95% exact binomial upper-bound method required by `latency_slo.v1`, so a
low-volume canary cannot hide a material error increase behind the absolute
percentage-point threshold.

The live reload drains existing pool entries under P2 rules; it does not kill
unrelated in-flight requests. Operators can set the relevant flag false to roll
back without restoring an old binary. Audit/metrics record cohort, flag
generation, disable reason, and the before/after sample window.

## Delivery sequence

1. **P0:** instrumentation, impairment profiles, and baseline.
2. **P1:** concurrent kb listener plus strict reusable framing.
3. **P2a:** server→kb pool, per-request identity/revocation, drain/retry tests.
4. **P2b:** reusable server HTTP/1.1 loop and reuse in naturally resident thin
   clients; keep short-lived CLI behavior explicit.
5. **P3:** gzip capability, safety limits, thin-client rollout; independently
   benchmark server→kb promotion.
6. **P4:** TLS context/session reuse inside resident processes.
7. **P6:** measured round-trip reduction where justified.
8. **P7:** staged canary enablement and automatic rollback for every promoted
   packet (the mechanism lands with P2; later packets reuse it).
9. **P5:** HTTP/2 only if its gate fires; otherwise close it as unnecessary.

Each packet is independently revertible. Capability negotiation lets either peer
fall back to uncompressed, one-request HTTP/1.1 during mixed-version rollout.

## Verification matrix

- Linux/OpenSSL thin client and server; macOS Secure Transport thin client;
  Windows Schannel or the documented TLS-terminating proxy topology.
- Old client/new server and new client/old server.
- mTLS optional and required modes; valid, expired, revoked, rotated, wrong-EKU,
  wrong-host, and unrecognized certificates.
- Cold/warm, sequential/concurrent, compressible/incompressible, tiny/large,
  response-before-close, close-before-response, half-close, restart, DNS change,
  and cert reload.
- Sensitive-route compression negative tests and decompression bomb/malformed
  stream tests.
- SSE/NDJSON byte-for-byte streaming and cancellation tests with compression off.
- Soak tests demonstrate stable worker count, descriptors, pool entries, RSS, and
  decoder/TLS object ownership.

## Non-goals

- No change to API payload semantics, tenancy, capability policy, certificate
  issuance, or the automatic mTLS rollout.
- No TLS compression or 0-RTT.
- No streaming HTTP content compression in P3; it requires the separately
  reviewed P3s packet.
- No in-tree HTTP/2, HPACK, QUIC, or HTTP/3 implementation.
- No gRPC migration merely to obtain multiplexing.
- No transparent proxy termination or trust in caller-supplied identity headers.
- No resident thin-client broker without its own authority and custody proposal.
- No compression of model context as a substitute for lossless HTTP content
  encoding; the context economizer is a different subsystem.

## Risks and rollback

- **Parser ambiguity:** reusable connections make framing bugs more dangerous.
  P1 refuses pipelining/chunked requests initially and fuzzes strict framing.
- **Revocation gap:** pooling could accidentally make handshake auth long-lived.
  Per-request durable checks and revoke-on-live-connection tests are load-bearing.
- **Compression side channel:** allowlist only, sensitive routes off, no
  secret+reflection compression.
- **Compression bomb:** incremental absolute and ratio caps before handler bytes.
- **Pool exhaustion/reconnect storm:** hard caps, bounded waiters, one replacement
  handshake per identity, no mutation retries.
- **Short-lived-client overclaim:** report handshake and compression improvements
  separately; do not attribute server warmth to cross-process socket reuse.

Rollback is configuration-first: set `transport.kb_pool_enabled=false`,
`transport.server_keepalive_enabled=false`, and the applicable gzip flag false to
restore one-shot, uncompressed requests; keep `http/1.1` ALPN. Live reload uses
the bounded drain rules above. Wire compatibility remains intact throughout.

## Convergence record

The initial architecture/security delegates and two-round concept review agreed
that:

- bounded kb concurrency and pooled HTTP/1.1 precede compression and HTTP/2;
- body compression must be explicit, per-link, thresholded, and bounded;
- application-layer per-request identity/revocation is the first-delivery mTLS
  mechanism under reuse;
- retries, pool ownership, rotation drain, decompression limits, measurement, and
  HTTP/2 admission need falsifiable contracts;
- HTTP/2 is deferred and must use a mature library.

This document incorporates those decisions. Failed/cancelled delegate seats were
treated as orchestration failures, never as votes, and were not replaced in
parallel. After the last corrections, sequential final audits `7609` (security)
and `7615` (QA/falsifiability) both returned `converged` with no blocking finding.
The proposal therefore reached the stated convergence criterion: no remaining
blocking correctness, security, performance, or original-request-alignment
finding across the completed final lenses.
