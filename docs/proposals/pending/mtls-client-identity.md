# mTLS client identity: per-client certs as attested principals

- **State:** proposed; awaiting roundtable + user proposal-gate.
- **Scope:** deterministic / server transport + auth + PKI lifecycle. Not an
  intelligence-surface proposal (no Architecture Charter role).
- **Author:** JBailes, 2026-06-17.
- **Builds on:** the native-TLS thin-client backends (Schannel/Secure
  Transport/OpenSSL behind `aimee_tls.h`) and the server's existing in-process
  TLS termination (`server_tls.c`, `server_api_tls_port`).

## Problem

Over the network, every aimee client authenticates with a **single shared
bearer token**. The bearer is anonymous and all-or-nothing: the server cannot
tell two TCP/TLS clients apart, and `aimee.api.remote_writes` gates the *whole*
listener, not per client. The current native-TLS path makes this explicit —
`ATTEST_TLS_BEARER` collapses every TLS+bearer connection to the **server**
principal (`server_tls.c`: "plain server TLS — the bearer is the authority").

Consequences:
- **No attribution.** Audit/`vault_audit_*` over the network records the server
  principal, not *who* acted.
- **No per-client scoping.** Every networked client gets the same capabilities
  and the same (server) vault; there is no per-developer vault or per-client
  `remote_writes` tier.
- **No targeted revocation.** A leaked bearer forces a rotation that breaks
  *every* client at once.

By contrast, the **local** UDS path already has per-user identity
(`ATTEST_UDS_PEERCRED` → `uid:<n>`), and webchat has `webuser:<name>`. The
network path is the one place identity degrades to a shared secret.

## Goal

Give each networked client an **individual, attested identity** via mutual TLS
with a **self-generated** (private) CA: a verified client certificate becomes a
distinct principal that the *existing* capability / per-principal-vault / audit
machinery keys off — so attribution, per-client scoping, per-client vaults, and
**revoke-one-without-rotating-everyone** all follow. No external/public PKI:
aimee owns both ends and trusts only its own CA.

## Why this fits aimee's existing model

The server already resolves a connection to an attested principal in **one
place** — `server_http_identity_capture()` calls
`vault_principal_resolve(is_tcp, is_tls, peer_uid, webuser, webuser_token_ok, …)`
and writes `conn->attested_transport` + `conn->vault_principal`. Everything
downstream (capabilities `vault_capability_*`, `remote_writes` gating,
per-principal vaults, audit) is already principal-keyed. mTLS adds **one more
identity source** to that resolver; the rest is reuse. The server also already
terminates TLS in-process (`server_tls_init`/`server_tls_accept`), so this is an
extension of an existing listener, not a new one.

## Design

### WP-A — Server: request + verify client certs
- Extend `server_tls_init` (`server_tls.c`) to, when mTLS is enabled, set
  `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, cb)`
  and `SSL_CTX_load_verify_locations(ctx, <config>/tls/client-ca.crt, NULL)` so
  the chain is verified against aimee's own CA. The verify `cb` also enforces
  revocation (WP-C) and is **fail-closed** (any verify error → handshake fails).
- New config `aimee.api.mtls` = `off` (default) | `optional` | `required`.
  `optional` requests a cert but still allows bearer-only (mixed fleet during
  rollout); `required` rejects a TLS connection with no/invalid client cert.
- After `SSL_accept`, expose the peer identity: new
  `server_tls_peer_identity(SSL *, char *out, size_t)` → the leaf cert's CN (or a
  designated SAN), plus its serial for revocation.

### WP-B — Identity wiring (the small, high-leverage change)
- New transport `ATTEST_MTLS_CLIENT` in `vault_principal.h`, mapping to a
  `cert:<CN>` principal (a namespace distinct from `uid:`/`webuser:`/server).
- In `server_http_identity_capture()`, when `is_tls` and a verified peer cert is
  present, pass its CN into `vault_principal_resolve`, which emits
  `(ATTEST_MTLS_CLIENT, "cert:<CN>")`. Fail-closed: `required` mode with no/invalid
  cert → empty principal (no access), exactly as a missed hop already collapses to
  `ATTEST_NONE`.
- No downstream changes: caps, `remote_writes`, vault, and audit consume the
  principal unchanged. Per-client `remote_writes`/capability grants become
  expressible because the principal is now per client.

### WP-C — Self-generated PKI lifecycle
- **CA**: an aimee-owned CA created on demand; its private key is **sealed in the
  existing server vault** (reuse `vault_crypto` / the server-master-key wrap) so
  it is never plaintext at rest. Issuance is operator-gated.
- **Commands**: `aimee cert issue <client-name>` (CN=`<client-name>`, short-lived,
  returns cert+key or signs a CSR), `aimee cert list`, `aimee cert revoke <name|serial>`.
- **Revocation = online DB1 serial denylist** checked in the verify callback (no
  CRL distribution problem; revoke takes effect on the next handshake). Fail-closed
  if the denylist can't be read.
- **Enrollment bootstrap**: a one-time, **bearer-authenticated** `/v1/cert/enroll`
  that issues a client cert (server-generated keypair or CSR sign), solving the
  first-hand-off trust problem by leaning on the existing shared bearer for
  enrollment only. After enrollment the client uses its cert, not the bearer.

### WP-D — Client: present a client certificate
The thin client loads its identity (`<aimee_home>/tls/client.{crt,key}`) and
presents it via each `aimee_tls.h` backend:
- OpenSSL (Linux): `SSL_CTX_use_certificate_chain_file` +
  `SSL_CTX_use_PrivateKey_file`. **DONE (slice 3).** Presented automatically when
  both files exist; a group/world-readable key is refused (fail closed). Gate is
  unit-tested (`unit-test-aimee-tls-clientcert`).
- Schannel (Windows): supply the client cert in `SCHANNEL_CRED.paCred` (imported
  from a PFX or the user cert store). **Deferred → slice 3b** (EC-key import via
  CryptoAPI must be validated on real Windows, as the native backend itself was).
- Secure Transport (macOS): `SSLSetCertificate` with a `SecIdentityRef` from a
  PKCS#12. **Deferred → slice 3b** (identity construction validated on real macOS).

This is the piece the native-TLS proposal explicitly deferred (mTLS was out of
scope there); it is additive, gated on the client actually having a cert. The
Linux leg lands first because it is the testbed for the whole mTLS loop
(server verify = slice 2, issuance CLI = slice 2b); the Windows/macOS legs ship
as slice 3b once validated on real hardware. Both native backends carry an
in-code marker at the exact hook point.

## Hardening requirements (from roundtable R1: architect + security + QA)

No lens found a design blocker; these are the must-address specifics before
implementation.

### Identity integrity (security-critical)
- **CN/SAN sanitization — prevent principal spoofing.** The `cert:<CN>` principal
  MUST be isolated from `uid:`/`webuser:`/server namespaces and rejected if the CN
  contains anything outside a strict charset (e.g. `[A-Za-z0-9._-]`, bounded
  length). A CN like `uid:0`, one embedding `:`/newlines/path-traversal, or one
  colliding with a real uid/webuser must be **refused at issuance** and, defensively,
  **re-validated at identity resolution** (a malformed/!charset CN → empty principal,
  fail-closed). The `cert:` prefix is applied by the server, never taken from the cert.
- **CN vs SAN precedence.** Pin exactly which field is the identity: use a single
  designated SAN (e.g. the first DNS/URI SAN) or the CN, documented and consistent;
  do not silently fall back in a way that lets a cert present two identities.

### Mode + downgrade
- **`optional` mode is a documented downgrade.** With `optional`, a leaked bearer
  can omit a client cert and revert to the shared server principal — losing
  attribution/revocation. Document this explicitly; recommend `required` for
  production. In `optional`, if a client **does** present a (valid) cert, the
  cert identity MUST win over bearer-only (no silently ignoring a presented cert).

### Enrollment + revocation
- **Enrollment is not just the shared bearer.** `/v1/cert/enroll` must add at least
  one of: operator approval, or **one-time, short-lived enrollment tokens** (not the
  long-lived shared bearer), so a bearer leak during rollout can't mint rogue client
  identities. Rate-limit + audit every enrollment. (Also define an out-of-band path
  for clients that can't present the bearer, e.g. CI runners — operator-issued cert.)
- **Revocation: atomic + available, still fail-closed.** The denylist read in the
  verify callback must be consistent (transactional/locked update; atomic read).
  Balance fail-closed against a DoS: a *transient* DB1 read error should fall back to
  a recent in-memory denylist snapshot (fail-closed against *known* revocations)
  rather than blocking **all** handshakes; a missing denylist store at startup is a
  hard config error. Define revocation propagation latency (single-server: immediate
  on next handshake).

### Lifecycle (CA + certs)
- **CA rotation procedure** (issue under a new CA, dual-trust both CAs during
  migration, reissue clients, retire the old CA) — must not break existing clients.
- **Cert expiry + renewal.** Short-lived client certs need `aimee cert renew` (or
  auto-renew before expiry); document the NTP/clock-skew expectation, and make the
  validity window wide enough that normal drift can't cause a fleet-wide outage.
- **PKI audit.** `aimee cert issue/revoke/renew` and enrollment each emit an audit
  record (who/when/serial/CN), never the key material.

### Client-side
- **Key protection + backend formats.** The client key file is created `0600`
  (refuse world-readable); optional passphrase. Document the per-backend cert source:
  OpenSSL PEM cert+key files, Schannel cert from a PFX/the user store, Secure
  Transport `SecIdentityRef` from a PKCS#12 — with a unified config knob and clear
  load-failure errors.

## Out of scope
- **Replacing the bearer.** mTLS + bearer coexist; `optional` mode keeps bearer-only
  clients working. The UDS path (peercred) is unchanged.
- **SSE/streaming over TLS** — already unsupported (`server_tls.h` phase 1c: an
  offloaded SSE worker can't share the `SSL`). Interactive `chat`/event streams over
  mTLS inherit that limitation until phase 1c lands; the data/RPC plane works now.
- External/public CAs, ACME, hardware-backed keys (HSM/TPM), cert transparency.

## Risks
- **CA key compromise = forge any client.** Mitigate: seal the CA key in the vault,
  operator-gate issuance, keep certs short-lived, support fast revocation. Consider
  keeping the CA key offline (issue out-of-band) for high-security deployments.
- **Enrollment bootstrap** still trusts the shared bearer for the *first* cert; a
  leaked bearer during the rollout window could enroll a rogue client. Scope `/v1/cert/enroll`
  tightly (rate-limit, audit, optionally operator-approve).
- **Streaming gap** (phase 1c): mTLS doesn't help interactive chat over the network
  until SSE-over-TLS exists; set expectations.
- **Fail-closed everywhere**: a verify-cb error, an unreadable denylist, or an empty
  CN must deny, never default-allow (mirror the existing `ATTEST_NONE`/no-`uid:0`
  discipline).
- **Principal namespace**: `cert:<CN>` must not collide with `uid:`/`webuser:`;
  CN uniqueness is enforced at issuance.
- **Clock skew** affects short-lived cert validity; document an NTP expectation.

## Acceptance criteria
1. A client presenting a valid aimee-CA-issued cert is identified as a distinct
   `cert:<CN>` principal; audit, capabilities, and the per-principal vault all key
   off it (two different client certs → two different principals).
2. `mtls=required`: a TLS connection with no client cert, an expired cert, a cert
   from an unknown CA, or a **revoked** serial is refused (fail-closed). `mtls=optional`:
   bearer-only still works.
3. Per-client scoping demonstrably works: one client can be granted a capability /
   `remote_writes` tier that another is not, and a revoked client loses access on its
   next connection without affecting others.
4. `aimee cert issue/list/revoke` manage the lifecycle; the CA private key is never
   written in plaintext (sealed in the vault).
5. Client-cert presentation works across all three backends (OpenSSL/Schannel/Secure
   Transport), validated like the native-TLS backends (CI build + a runtime mTLS
   handshake test against a local aimee CA), including the per-backend cert source
   (PEM / PFX / PKCS#12).
6. **CN/SAN spoofing is rejected**: a cert whose CN is `uid:0`, contains out-of-charset
   bytes (`:`/newline/path-traversal), or collides with an existing `uid:`/`webuser:`
   principal is refused at issuance and yields an empty principal at resolution.
7. **`optional` mode**: a client that presents a valid cert is identified by it (cert
   identity wins over bearer); a leaked-bearer client with no cert gets only the
   downgraded server principal — and this downgrade is documented.
8. **Enrollment** with a one-time/short-lived token (or operator approval) issues a
   cert; the long-lived shared bearer alone does not, beyond the documented bootstrap
   window; every issue/revoke/renew is audited (no key material in the log).
9. **Revocation availability**: a transient denylist read error still rejects a
   known-revoked serial (snapshot fallback) without blocking valid handshakes; a CA
   rotation reissues clients without breaking already-trusted ones.
