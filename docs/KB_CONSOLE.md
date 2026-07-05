# aimee-kb web console

`kb-console` is a standalone Go thin-client that fronts a shared **aimee-kb**'s
`/v1` surface directly, so a company-wide KB is administrable from a browser with
**no colocated `aimee-server`**. It mirrors `aimee-webchat`'s shape (auto-TLS
HTTPS, SQLite sessions, an `/api/*` proxy, a vendored-SmoothGUI single-file SPA)
but uses **no PAM**: login is OIDC, and it holds a scoped credential the kb
enforces server-side.

Surfaces: **Dashboard** (kb health/throughput), **Accounts** (client enrollment,
certificate revocation, scopes, OIDC config), **Governance** (decision records,
the policy-verdict action audit). This document is the trust model + operations
guide (see `docs/proposals/done/kb-web-console.md` and its `.plan.md` for the design).

## Status

**Shipped, default-off.** The console is a separate opt-in service (its own
`Dockerfile.kb-console` + the compose `console` profile) that only runs when
launched with a console-admin credential file, bound to `127.0.0.1` by default.
The Dashboard, Accounts (enroll / revoke / scopes / OIDC config), and Governance
(decisions / action audit) surfaces are all live. The curator review queue is the
one deferred surface (it needs a separate curator-scoped credential).

## Trust model

The console is a **privilege-escalation surface** and is treated as fail-closed,
not merely "semi-trusted".

- **Scoped console-admin credential, not owner.** The console holds a
  `scope:console-admin:<id>:<secret>` bearer whose route allowlist the **kb
  enforces server-side** (`src/kb/http/kb_route_acl.c`): only
  `POST /v1/enroll` (mint), `/v1/console/overview`, `/v1/enrollments` (+ revoke),
  `/v1/config/oidc`, `/v1/scopes`, `/v1/decisions` (+ sub-actions), and
  `/v1/audit/actions`. A compromised console is bounded to that allowlist. It may
  mint client enrollments, but the kb **refuses to mint an owner or privileged
  scope for a console-admin caller** — the requested scope must be a proper
  `<kind>:<id>` and the kind may not be `owner`/`console-admin`/`curator` — so the
  console cannot escalate by minting. `/v1/review` is **not** in the set: review
  accept/reject is a separate `curator` scope. (The console is on the kb's compose
  network, so other services on it can reach the console port, but every action
  still requires a valid console session — OIDC or break-glass.)
- **Deny-by-default proxy.** `/api/*` maps 1:1 to the allowlisted `/v1` routes and
  re-checks the same allowlist in Go (`acl.go`) before forwarding — defence in
  depth with the kb's server-side check. The browser's own token is never
  forwarded; only the console-admin bearer, server-side.
- **Login = OIDC primary.** The console verifies a browser-presented OIDC JWT
  itself (RS256 only, `iss`/`aud`/`exp`/`nbf` checked, JWKS by `kid`) and requires
  a pinned admin claim. This is a port of `src/kb/auth_oidc.c`; a parity test
  locks the claim mapping.
- **Break-glass is off by default.** A static console-admin bearer login is
  enabled only when the operator drops a presence-flag file
  (`$KB_CONSOLE_HOME/.break_glass`, mode 0600). Break-glass sessions are capped at
  **300 s** and audited (console-local record now; a kb-side `audit_events` row is
  added with the auth endpoints in S2a). Used to recover from an OIDC
  misconfiguration lockout.
- **Sessions + CSRF.** Cookies are `HttpOnly; Secure; SameSite=Strict`; every
  mutating `/api/*` call requires the per-session CSRF token in an `X-CSRF-Token`
  header, compared server-side to the session's stored token (a synchronizer-token
  pattern — the `SameSite=Strict` cookie is the primary CSRF defence, the header is
  defence-in-depth). Login is rate-limited per source IP. A
  session is bound to `(iss, sub)` — not `sub` alone — so it is not portable
  across IdPs, and revoking a principal's enrollment invalidates its sessions.
  Idle (30 min) and absolute (8 h) timeouts apply. The SQLite session DB is mode
  0600.
- **Credential storage.** The console-admin bearer is read from
  `KB_CONSOLE_CRED_FILE` (mode 0600); the console **refuses to start** if the file
  is group/world-readable, or if an inline `KB_CONSOLE_CRED` env var is set.
- **CSP.** The SPA is served with a strict Content-Security-Policy: `default-src
  'self'`, no `'unsafe-inline'`, no `*`. The single-file bundle's inline
  script/style are allowed by pinning their exact sha256 hashes (computed once at
  startup); the only non-self entries are the IdP origin (for the OIDC flow).
- **Fail-fast startup.** On boot the console probes `GET /v1/console/overview`
  with its credential and refuses to start unless it 200s (credential + ACL +
  route all wired).

## Certificate revocation semantics (S2a)

Revoking an enrollment (`POST /v1/enrollments/{id}/revoke`) sets `revoked_at` in
DB2 — the **source of truth** — and the mTLS seam rejects a revoked client cert on
its next request (matched by the sha256 fingerprint of the cert DER). Two deliberate
trade-offs:

- **Fail-open on a DB outage, for unknown certs only.** If DB2 is unreachable, an
  *unknown* fingerprint is treated as not-revoked so a transient DB blip cannot lock
  every mTLS client out of the shared kb. A **known** revocation still holds through
  an outage: `revoke` primes the in-process cache with the revoked verdict, which is
  checked before the DB. Every fail-open is logged (throttled) as a `WARN`.
- **Single-instance cache.** The is-revoked cache is per-process with a 30 s TTL; a
  revoke is reflected immediately in the process that served it. Running multiple kb
  instances means a revoke can take up to the TTL to be seen by the others — run a
  single kb instance, or shorten the TTL, for a tight revocation window.

Certs issued before S2a (no enrollment row) are **backfilled** as `legacy` rows on
their first authenticated request, so they become listable and revocable.

## Running (dev)

```bash
# 1. Mint a console-admin enrollment on the kb (owner credential):
#    POST /v1/enroll {"scope":"console-admin", ...}  ->  the scoped bearer.
printf '%s' "$CONSOLE_ADMIN_BEARER" > console.cred && chmod 600 console.cred

# 2. (optional) OIDC config file:
cat > oidc.json <<'JSON'
{ "issuer": "https://idp.example.com", "audience": "aimee-kb-console",
  "jwks_url": "https://idp.example.com/jwks", "admin_claim": "groups",
  "admin_values": ["aimee-admins"] }
JSON

# 3. Run (localhost-only, auto-TLS):
kb-console -kb https://aimee-kb:8741 -cred console.cred -oidc oidc.json
```

Without an OIDC file the console runs **break-glass-only** — create
`$KB_CONSOLE_HOME/.break_glass` (0600) to enable the recovery login.

## Deploy (compose)

The console is a **default-off** compose service under the `console` profile — a
stock `docker compose up` never starts it. To enable it:

```bash
# 1. Mint a console-admin enrollment on the kb (owner credential) and save it:
printf '%s' "$CONSOLE_ADMIN_BEARER" > console.cred && chmod 600 console.cred

# 2. Bring the stack up WITH the console profile:
docker compose --profile console up -d
#   The console is published on 127.0.0.1:8744 (localhost-only), fronts the kb at
#   http://aimee-kb:8741, and reads ./console.cred (override KB_CONSOLE_CRED_FILE).
```

Image: `Dockerfile.kb-console` (a Node stage builds the SPA, a Go stage builds the
binary, a slim runtime). Override `AIMEE_KB_CONSOLE_IMAGE` to pin a published tag.

Configure OIDC login through the **Accounts → OIDC login config** editor (stored in
the kb's DB2); **restart the console** to apply. Until OIDC is configured the
console is break-glass-only (drop `$KB_CONSOLE_HOME/.break_glass`, mode 0600).

## The curator review queue (not in the console)

The curator review queue (`GET /v1/review`, `POST /v1/review/{id}/accept|reject`)
is **not** exposed by the console: it is a **curator-scope** action, and folding it
into the console-admin credential would break the separation of duties (a
console-admin could self-approve changes to the very config it administers). The
kb's built-in verifier derives scope from the single configured bearer, so a
distinct curator scope only exists over **mTLS** today — there is no clean
curator-credential path over the console's HTTP+bearer transport.

Until the kb gains multiple scoped bearers, work the review queue directly with a
curator credential (an mTLS client cert whose CN is `curator:<id>`):

```bash
# list pending review items
curl -fsS --cert curator.pem --key curator.key https://aimee-kb:8743/v1/review
# accept / reject one
curl -fsS -X POST --cert curator.pem --key curator.key \
  https://aimee-kb:8743/v1/review/<id>/accept
```

A console **OIDC config** change (Accounts → OIDC login config) applies on the
next **console restart** — there is no live reload.
