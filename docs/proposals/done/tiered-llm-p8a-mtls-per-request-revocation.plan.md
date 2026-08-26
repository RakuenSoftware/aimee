# P8a implementation plan: per-request durable revocation/expiry re-check (P8 §3)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice P8a of P8 (thin-client↔server mTLS). Branch off `testing`. P8's server-side mTLS is
mostly "wiring, not green-field" (server_api_mtls 0/1/2, mtls_verify_cb, client TLS+TOFU,
the enrollment CA, cert:CN principal all exist). P8a ships the ONE load-bearing security
core the proposal calls "the load-bearing test for invariant #5": the **per-request durable
revocation + expiry re-check** on an already-established mTLS connection. It is server-side
(DB1/SQLite, aimee-server), offline unit-testable, and independent of a live two-node
handshake or the client-presents-cert / ramp / enrollment work (P8b/c).

## Verified substrate (from the substrate map)

- **`src/server/pki.c`**: durable table `pki_certs(serial TEXT PK, cn, issued_at,
  expires_at, revoked INT)`. `pki_is_revoked(serial)` reads an **IN-MEMORY snapshot only**
  (`g_revoked[]`, loaded once at startup via `snapshot_load()`, kept current in-process by
  `snapshot_add()` on each `pki_revoke()`). No per-request DB re-read exists.
- **`src/server/server_tls.c`**: `mtls_verify_cb` runs ONLY at handshake (OpenSSL cannot
  re-run it on a keep-alive/HTTP-2 connection). `server_tls_peer_identity(ssl, cn, cn_len,
  serial, serial_len)` extracts the peer CN + hex serial post-handshake.
- **`src/server/server_http.c`**: `handle_conn`: identity capture at :1808
  (`server_http_identity_capture`) → route dispatch at :1810 (`server_http_route`). The
  natural slot for the re-check is BETWEEN these. `send_response(fd, code, json, request_id)`
  is the rejection mechanism (401/403 precedents at :1493/:1545).
- **`src/server/server_http_identity.c`**: the mTLS hop extracts the serial into a local
  but **DISCARDS it** (only `cert:<CN>` survives). `attested_transport_t` includes
  `ATTEST_MTLS_CLIENT` (the cert class), set on `conn->attested_transport`.
- **Test:** `src/tests/test_pki.c` (+ `unit-test-pki` in Rules.mk:321/:3399) already proves
  revocation persists across a snapshot reload, the pattern to extend.

## The gap (invariant #5)

`mtls_verify_cb` catches a revoked cert AT HANDSHAKE. But on a persistent keep-alive/HTTP-2
connection, subsequent requests do NOT re-run the callback, so a client revoked via
`cert.revoke` AFTER its handshake keeps authorizing until it reconnects. Compounding it, the
only revocation check (`pki_is_revoked`) reads an in-memory snapshot, which the proposal
explicitly forbids as the per-request source ("no in-memory cache that could miss a
just-revoked cert"); P8a closes both: a **fresh, durable** per-request re-check.

## Design decisions

1. **`pki_cert_check(serial, now)`, a fresh DURABLE query, the per-request authority.**
   New `src/server/pki.c` function that queries the DB directly (`SELECT revoked, expires_at
   FROM pki_certs WHERE serial = ?`) (NOT the in-memory snapshot) and returns a typed
   status: `PKI_CERT_VALID` / `PKI_CERT_REVOKED` / `PKI_CERT_EXPIRED` (expires_at>0 AND
   expires_at<=now) / `PKI_CERT_UNKNOWN` (serial not in pki_certs. A cert this server's CA
   did not issue/record). It re-reads the durable source on every call. A revocation
   written to `pki_certs` is authoritative on the very next request, with no snapshot
   staleness. (`pki_is_revoked` stays as the fast handshake-path snapshot check; P8a ADDS
   the durable per-request check, doesn't replace it.)
2. **Per-request re-check in `handle_conn`, between identity capture and routing.** When the
   connection is mTLS-attested (`conn->attested_transport == ATTEST_MTLS_CLIENT`), re-extract
   the peer cert serial (`server_tls_peer_identity` on the conn's SSL) and call
   `pki_cert_check(serial, now)`. On REVOKED / EXPIRED / UNKNOWN → `send_response(fd, 403,
   …, request_id)` and RETURN, the request never reaches routing. VALID → proceed. This
   fires on EVERY request of a persistent connection, so a post-handshake revocation stops
   authorizing on the next request. **Fail-closed**: if the conn claims MTLS_CLIENT but the
   serial cannot be re-extracted, refuse (403). A non-mTLS connection (bearer/UDS) is
   unaffected.
3. **Serial-keyed, single server CA (honest scope).** The server owns one client CA and
   `pki_certs.serial` is the PK, so serial uniquely identifies a leaf here. P8a keys the
   re-check on serial (matching `mtls_verify_cb` + `cert.revoke` today). The fuller
   `(cert_issuer, serial)` compound key + the durable `(issuer,serial)→principal+capabilities`
   enrollment record (P8 §1/§3) are a documented **P8b** follow-up (they need an `issuer`
   column + a capability-binding schema, out of scope here).
4. **No capability change (P8 invariant).** P8a only ADDS a revocation/expiry refusal; it
   never grants a cert client any capability. `remote_writes` stays UDS-only; the caps gate
   is untouched.

## Scope (P8a)

1. `src/server/pki.c` + `src/headers/pki.h`: `pki_cert_check(const char *serial, long now)`
   → `pki_cert_status_t` (VALID/REVOKED/EXPIRED/UNKNOWN), a fresh durable DB read. A small
   `pki_cert_status_str()` for logging.
2. `src/server/server_http.c` (`handle_conn`): the per-request mTLS re-check between :1808
   and :1810, refuse (403) on non-VALID for an `ATTEST_MTLS_CLIENT` conn, fail-closed on a
   missing serial.
3. `src/tests/test_pki.c` (extend) OR a new `test_pki_reauth.c` + `unit-test-pki-reauth`:
   (a) `pki_cert_check` returns VALID for a fresh issued cert; (b) after `pki_revoke` →
   REVOKED; (c) an expired cert (expires_at in the past) → EXPIRED; (d) an unknown serial →
   UNKNOWN; (e) **the load-bearing property**: with the in-memory snapshot deliberately
   STALE (revoke the row directly in the DB WITHOUT `snapshot_add`, or via `pki_revoke` then
   assert), `pki_is_revoked` may miss it but `pki_cert_check` returns REVOKED, proving the
   durable per-request check is authoritative where the snapshot is not; (f) now-boundary:
   expires_at == now is treated per the chosen `<=` rule (document which).

## Explicitly deferred (P8b/c)

The client-presents-cert wiring (`aimee_tls.c` `SSL_CTX_use_certificate`, 0600 key install
crash-safe); the ramp state machine + auto-advance `1→2` + the observable "not-yet-required"
signal (§3/§4); per-client enrollment (§1); the bounded bearer floor-capability set in
optional mode; the `(cert_issuer, serial)` compound key + the durable enrollment
record→principal+capability binding; the EKU clientAuth-only cross-profile test; the
integration two-client enroll/revoke/handshake test. P8a is the server-side per-request
durable revocation/expiry re-check, the invariant-#5 core those build around.

## Gate

- `make -j server` links clean (server + kb). No .sql schema in DB2 changes (this is DB1);
  `make schema-sync-check` unaffected.
- `make lint` (line-check, module-boundary, etc.) green; new/changed files clang-format-19
  clean.
- `unit-test-pki` (extended) or `unit-test-pki-reauth` builds + PASSES. The durable-beats-
  stale-snapshot property (e) is the headline.
- No real-PG gate needed (server-side DB1/SQLite); run-p1-rls-gate.sh untouched.

## Non-goals (P8a)

No client cert presentation, no ramp/auto-advance, no enrollment, no capability change, no
issuer-compound-key, no integration handshake test, no bearer-floor set. Pure server-side
per-request durable revocation/expiry re-check that closes the keep-alive-after-revoke gap
for invariant #5, unit-tested offline.

## v2 refinements (roundtable-converged; fail-closed completeness + honest framing)

- **A distinct `PKI_CERT_ERROR` state, never conflate an unavailable authority with an
  unknown cert.** The status enum becomes VALID / REVOKED / EXPIRED / UNKNOWN / **ERROR**
  (a SQLite prepare/bind/step/lock failure). BOTH `UNKNOWN` and `ERROR` **fail closed** (the
  request is refused 403. A revocation authority that cannot answer must never fail open),
  but they are **logged distinctly** so an operator can tell "cert not on this server's
  roster" from "revocation store unavailable." (A transient DB error must not be reported as
  a permanent, misleading UNKNOWN.)
- **Q2, answered in the artifact (not deferred):** the fix is BOTH parts. The **load-bearing**
  fix is re-checking PER REQUEST at all, `mtls_verify_cb` runs only at handshake and OpenSSL
  cannot re-run it on a keep-alive connection, so without a per-request re-check a
  post-handshake `cert.revoke` keeps authorizing. Reading the **durable source** (not the
  `g_revoked[]` snapshot) is the **mandated correctness posture** (the proposal forbids "an
  in-memory cache that could miss a just-revoked cert"): for a single process the snapshot
  is usually current, but the durable read removes any snapshot-divergence assumption and is
  authoritative under a DB-restore / future multi-reader. P8a does both.
- **Serial-format consistency (implementation contract).** The serial passed to
  `pki_cert_check` MUST be the exact `BN_bn2hex` form that `pki_issue` writes to
  `pki_certs.serial` and that `mtls_verify_cb` / `pki_is_revoked` already use, i.e.
  re-extract it via `server_tls_peer_identity` (same code path), so the DB `WHERE serial = ?`
  lookup can never miss on a format mismatch (a format skew would silently fail OPEN as
  UNKNOWN→refuse, which is safe, but must be avoided to not lock out valid certs).
- **Expiry semantics + authority (documented).** The boundary rule is `expires_at > 0 AND
  expires_at <= now → EXPIRED` (an unset/0 expires_at is not treated as expired). The DB
  `expires_at` is the value recorded from the cert's signed `notAfter` at `pki_issue`; the
  cert's signed `notAfter` remains the ultimate authority, and OpenSSL already rejects an
  expired cert at handshake. The app-layer re-check exists ONLY to catch a keep-alive
  connection that crosses the expiry boundary mid-session. (Cross-checking the live peer
  cert's `notAfter` is a P8b nicety; P8a uses the recorded value, which equals it at issue.)
- **Test harness decided: EXTEND `test_pki.c`** (its `unit-test-pki` recipe already links
  `pki.o` + `$(DB1_OBJS)`, no new Rules.mk plumbing). No separate `unit-test-pki-reauth`.
- **HTTP/1.1-only (no bypass concern).** aimee's /v1 server is hand-rolled HTTP/1.1 (ALPN
  advertises h1 only), so there is no HTTP/2 multiplex/per-stream re-check subtlety; the
  single re-check between identity-capture and route-dispatch covers every request. The
  `pki_cert_check` call is a single prepared `SELECT` on a PK, cheap at single-server scale.

### Gate additions

- Test (g): `pki_cert_check` on a simulated DB-unavailable path returns `PKI_CERT_ERROR`
  (and the handler would refuse). The authority-down case fails closed, not open.
