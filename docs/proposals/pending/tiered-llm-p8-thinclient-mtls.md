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

## §1 Per-client enrollment

Issue one cert per thin-client via the existing CA. A thin-client enrolls with a
single-use token (mirrors `aimee remote set`'s pin step), submits a CSR, and
receives a CA-signed client cert whose `CN` identifies the client (and, post-P1,
maps to a user/team). The private key never leaves the client; the server stores
no client secret. Rotation = re-enroll; revocation = `cert.revoke` on that one
`CN`, affecting no other client.

## §2 Client presents the cert

Extend `aimee_tls.c` to load and present the client cert/key on connect
(`SSL_CTX_use_certificate` / `_PrivateKey`), stored 0600 alongside
`remote-ca.pem`. The connection is now mutually authenticated: client verifies
server (existing TOFU pin), server verifies client (`mtls_verify_cb`).

## §3 Server requires mTLS (with a graceful ramp)

- **Ramp:** move deployments `0 → 1 (optional) → 2 (required)`. Optional mode
  lets a fleet enroll certs while bearer still works; required mode is the
  hardened end state.
- In **required** mode the client is authenticated as `cert:<CN>`, and the
  shared bearer is no longer accepted for the data plane (or is kept only as a
  documented break-glass, matching kb-console's break-glass pattern).
- Capability gating (`server_http_conn_caps`, `server_http.c:334-347`) can grant
  a `cert:CN`-attested client more than a bearer-only one — e.g. `remote_writes`
  without the current UDS-only restriction, since the caller is now strongly
  identified.

## §4 Shipped-config defaults

Update the remote-first compose/deploy configs to provision a client-CA and
default `server_api_mtls: 1` (optional) so new deployments enroll cleanly, with
a documented switch to `2` (required) for hardened installs. Plaintext 8740
stays loopback-only (unchanged), as does the self-signed server-TLS
auto-provisioning (unchanged).

## Acceptance criteria

- A thin-client enrolls, receives an individual cert, and connects mTLS; a
  second client gets a *different* cert; revoking one does not affect the other.
- In `required` mode a bearer-only client is refused; a valid client cert is
  accepted and identified as `cert:<CN>`.
- The client still fails closed if the server cert doesn't match its pin
  (existing behaviour preserved) — mutual verification, both directions.
- `optional` mode still accepts a bearer client (documented downgrade), so a
  fleet can migrate without an outage.
- A `cert:CN`-attested client can perform a capability that a bearer client
  cannot (proves the stronger transport is recognised).

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
