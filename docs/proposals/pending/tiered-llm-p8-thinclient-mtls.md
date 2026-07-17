# Proposal: P8 — mTLS on the thin-client ↔ server link (per-client certs)

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** the enrollment CA (`src/kb/enroll.c`, `src/server/server_cert.c`)
  — reused, not rebuilt. Complements the always-mTLS kb↔server invariant (P2/P5).

## Thesis

The `aimee` CLI thin-client ↔ server link is TLS-encrypted with TOFU **server**-cert
pinning and a **shared bearer** for client auth, but is not mutually
cert-authenticated. No technical reason for that gap: the thin-client is a program
(not a browser), the server already has full mTLS machinery on this exact link, and
aimee already runs the enrollment flow to issue per-client certs. mTLS here is a
default to flip, not a capability to build. This packet makes **per-client mTLS
the required posture** on the thin-client↔server link, matching the
machine-identity spine used everywhere else.

## Goal

Every `aimee` thin-client authenticates to its server with its **own individual
mTLS client cert** (unique `cert:CN`, individually revocable). The shared bearer
becomes a fallback; hardened deployments disable it. Enrollment, rotation, and
revocation reuse the existing CA flow.

## §0 What already exists (so this is wiring, not green-field)

- **Server-side mTLS on this link is already implemented** — `server_api_mtls`
  (`config.h:1333-1338`): `0=off`, `1=optional` (request client cert, bearer still
  works — "a documented downgrade"), `2=required`. Verify callback
  `mtls_verify_cb` (`src/server/server_tls.c:31-54`); client-cert
  issuance/revocation in `src/server/server_cert.c`. **Default is `0` today** —
  that is the only reason the link is bearer-only.
- **The client already does TLS + TOFU server-cert pinning** — `src/aimee_tls.c`
  (`SSL_VERIFY_PEER`, hostname/SAN fail-closed, pins `remote-ca.pem`), driven by
  `remote_pin_cert()` / `aimee remote set` (`src/cli_remote.c:150-305`). Making
  it *mutual* means the client additionally presents a client cert — a strictly
  additive change to a path that already verifies the server.
- **Enrollment machinery exists** — the `aimee://` single-use-token → CSR →
  CA-signed `cert:CN` flow (`src/kb/enroll.c`, `kb_enroll_mint`/`_redeem_csr`),
  and the server's own `cert.issue`/`cert.list`/`cert.revoke`
  (`server_auth.c:108-110`, `server_cert.c`). Per-client certs slot straight into
  this.
- **`cert:CN` is already a first-class principal** — `src/headers/vault_principal.h`,
  and the attested-transport gate already recognises `ATTEST_TLS_BEARER`
  (`server_http_identity.c:113-123`). mTLS adds a `cert:CN`-attested transport
  class that is *stronger* than bearer, so capability gating can grant it more.
- **Distinct cert profiles already exist — reusing the CA does not blur them.**
  The CA issues thin-client certs with EKU `clientAuth` and server certs with EKU
  `serverAuth` (`src/kb/pki.c:240` vs `:315`; the CSR-signing enrollment path
  `kb_pki_sign_csr` also stamps `clientAuth`, `:422`), and the leaf CN is
  server-controlled (the CSR's own subject is ignored — verify-then-trust,
  `pki.c:411`). So a thin-client cert can never satisfy a `serverAuth` purpose, and
  the thin-client's trust of the *server* stays on its separate TOFU pin
  (`remote-ca.pem`), not the enrollment-CA chain. This packet issues only
  `clientAuth` leaf certs and adds no server-identity capability; P5's fleet-mgmt
  channel likewise stays on `serverAuth`/`clientAuth` per its direction.

## §1 Per-client enrollment

Issue one cert per thin-client via **one defined trust domain** — the thin-client
enrolls against the **server's own enrollment CA / endpoint** (`server_cert.c`
`cert.issue`), distinct from kb's fleet-enrollment CA; the packet does not conflate the
two. A thin-client enrolls with a single-use token (mirrors `aimee remote set`'s pin
step), submits a CSR, and receives a CA-signed client cert whose `CN` identifies the
client (and, post-P1, maps to a user/team **as a label only** — a client cert is *not*
a kb-verifiable actor token, so it cannot by itself make a forwarded human identity
authoritative for org egress; that still requires the actor token of P2/P9). The
**uniqueness/revocation key is the immutable `(cert_issuer, serial)`** (CN is the
policy label), so identity collisions are impossible and `cert.revoke` targets exactly
one leaf. The private key never leaves the client; the server stores no client secret.
Rotation = re-enroll; revocation affects no other client.

## §2 Client presents the cert

Extend `aimee_tls.c` to load and present the client cert/key on connect
(`SSL_CTX_use_certificate` / `_PrivateKey`), stored 0600 alongside
`remote-ca.pem`. The connection is now mutually authenticated: client verifies
server (existing TOFU pin), server verifies client (`mtls_verify_cb`).

## §3 Server requires mTLS (with a graceful ramp)

- **Ramp:** move deployments `0 → 1 (optional) → 2 (required)`. Optional mode
  lets a fleet enroll certs while bearer still works; required mode is the
  hardened end state.
- In **required** mode the client is authenticated as `cert:<CN>` and a bearer-only
  data-plane client is **unambiguously refused** — the shared bearer is not accepted on
  the data plane at all (no break-glass downgrade on the data plane; see the bounded-fallback
  bullet below for the only permitted bearer use).
- **Per-request revocation mechanism** (not just the handshake callback): the server
  re-checks the client cert's `(issuer, serial)` against a **fresh revocation source on
  every request** over a keep-alive / HTTP-2 connection — its own primary-backed
  revocation list (the server owns this CA) — so a revoked client stops authorizing on
  its next request, not only at the next handshake. Because OpenSSL cannot re-run
  `mtls_verify_cb` on an already-established keep-alive / HTTP-2 connection, the
  **application request handler** re-validates the peer's `(cert_issuer, serial)` —
  revocation, expiry, and chain — on every request; the revocation source is the server's
  own **local, durable revocation list** (the server owns this CA), updated via its own
  `cert.revoke` API and **re-read from the durable source on every request** — no
  in-memory cache that could miss a just-revoked cert. (This link is a single-user
  server↔its-own-client, so the source is the server's local store, not kb's DB2.) The
  `(cert_issuer, serial)` → authorization-principal mapping is locked **in the same
  enrollment record that grants the principal its capabilities**, so a renewed/rotated
  cert cannot silently inherit or change authority.
- The **client key is installed crash-safely** — created `O_CREAT|O_EXCL` at `0600`,
  written to a temp file, `fsync`ed, atomically renamed — never a world-readable window.
  **Threat model, explicit:** the key is bound to the client machine's OS account; the
  protection is against other local accounts and casual copy, not against a root/owner of
  that machine (who is the legitimate client) — a stolen key is contained by `cert.revoke`.
- **`remote_writes` stays UDS-only.** A `cert:CN`-attested *thin-client* does **not** gain
  `remote_writes` over the network — that would enlarge the attack surface (a stolen client
  key → remote config writes); privileged writes continue to go through the kb→server
  control plane (P5), not the thin-client link. The thin-client mTLS win is strong
  *authentication and per-request revocation*, not capability elevation.
- **Auto-advance is a durable state machine:** `(ramp_state, roster_hash, last_advance_ts)`
  persisted (SQLite row, `fsync`ed) with a startup self-test; "registered" = enrolled in
  the server's `cert.list` roster; readiness = every enrolled client has presented a valid
  cert **or** an explicit operator command advances it — observable, not self-referential.
  A **hardened deployment ships `required`** because its provisioning tool generates the
  initial client certs **out-of-band before boot** (so there is no bearer window); only an
  upgrade of an existing bearer fleet uses the bounded `optional` ramp.
- **Bearer is a bounded, audit-logged fallback with explicit operator opt-in**, capped to
  an enumerated floor capability set (read-only session ops — no `remote_writes`, no config
  mutation); a test asserts no data-plane write endpoint is reachable by a bearer-only
  client in optional mode. No vague "break-glass": it is either off, or a concretely-defined
  restricted listener — not an unspecified escape hatch.
- **kb-egress attribution for a thin-client-driven call:** the *server's* `cert:CN` is the
  authoritative origin at kb (invariant #7); the thin-client's identity is authoritative
  only if it rides a kb-verifiable actor token — a client cert alone does not make the
  forwarded human identity authoritative.
- Capability gating (`server_http_conn_caps`, `server_http.c:334-347`) can grant
  a `cert:CN`-attested client more than a bearer-only one — e.g. `remote_writes`
  without the current UDS-only restriction, since the caller is now strongly
  identified. **In `optional` mode this asymmetry is enforced, not incidental:** a
  bearer-only client is capped to a floor capability set and **never** receives the
  elevated capabilities (`remote_writes` etc.) reserved for `cert:CN`-attested
  callers — so accepting bearer during the migration window cannot be used to
  exercise cert-level privilege. Elevation strictly requires cert attestation.

## §4 Shipped-config defaults

Update the remote-first compose/deploy configs to provision a client-CA. Optional
mode (`1`) is a **bounded migration state, not a resting state**: a fresh deployment
enrolls its clients in optional mode, then **auto-advances to `2` (required)** once
all known clients hold certs (or at the end of a defined enrollment window), and
**new hardened deployments ship `2` (required)** by default. While in optional mode
the server emits an observable "bearer still accepted — not yet at required posture"
signal, so a deployment cannot silently rest in an indefinitely bearer-accepting
state (which invariant #5 forbids as the end state). Auto-advance to `required` is
gated on an explicit "all registered clients hold valid certs" signal (or operator
confirmation); a failed/rejected enrollment holds the ramp and alerts rather than
flipping to `required` and locking a client out. The **authoritative client roster is
the server's own enrollment records** (`cert.list`) — "registered" = enrolled there —
and the transition is a durable, idempotent algorithm (a persisted ramp state advanced
only when the roster is fully enrolled), so a shared-bearer deployment can concretely
determine readiness rather than guessing. Plaintext 8740
stays loopback-only (unchanged), as does the self-signed server-TLS
auto-provisioning (unchanged).

## Acceptance criteria

- A thin-client enrolls, receives an individual cert, and connects mTLS; a
  second client gets a *different* cert; revoking one does not affect the other.
- In `required` mode a bearer-only client is refused; a valid client cert is
  accepted and identified as `cert:<CN>`.
- Revoking a thin-client cert stops it authorizing on its **next request**, not only
  at the next handshake — an already-established keep-alive connection is
  re-checked per request against revocation, so it cannot keep authorizing after
  `cert.revoke`.
- The client still fails closed if the server cert doesn't match its pin
  (existing behaviour preserved) — mutual verification, both directions.
- `optional` mode still accepts a bearer client (documented downgrade), so a
  fleet can migrate without an outage.
- A `cert:CN`-attested client can perform a capability that a bearer client
  cannot (proves the stronger transport is recognised).
- The shipped hardened default is `required`; a deployment left in `optional`
  surfaces a visible "not-yet-required" signal and auto-advances to `required` once
  its clients are enrolled — bearer-only is not an indefinite accepted posture.
- A thin-client leaf cert carries EKU `clientAuth` only and is rejected if offered
  as a server cert (the `clientAuth`/`serverAuth` profiles do not cross).
- **Revocation on a live connection:** a cert revoked via `cert.revoke` is refused on the
  **next request** of an existing keep-alive/HTTP-2 connection (the load-bearing test for
  invariant #5's guarantee), not only at re-handshake.
- A bearer-only client in `optional` mode reaches **no** data-plane write endpoint (floor
  capability set enforced); the durable ramp state survives a server restart (crash self-test).

## Testing

Unit: `mtls_verify_cb` accept/reject (valid CA-signed / self-signed / revoked /
wrong-CN), client cert presentation, cap elevation for `cert:CN`. Integration:
enroll two clients against a real server, complete the mTLS handshake for both,
revoke one and prove it is rejected while the other still connects; ramp `1→2`
and prove bearer-only is refused at `2`.

## Non-goals

No browser/webchat change — webchat reaches the server over UDS (loopback), not
this remote link, so it is out of scope. No removal of TOFU server-cert pinning
(it stays — mTLS is additive). No new CA — reuse the enrollment CA.
