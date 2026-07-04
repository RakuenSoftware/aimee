# aimee-kb web console

`kb-console` is a standalone Go thin-client that fronts a shared **aimee-kb**'s
`/v1` surface directly, so a company-wide KB is administrable from a browser with
**no colocated `aimee-server`**. It mirrors `aimee-webchat`'s shape (auto-TLS
HTTPS, SQLite sessions, an `/api/*` proxy, a vendored-SmoothGUI single-file SPA)
but uses **no PAM**: login is OIDC, and it holds a scoped credential the kb
enforces server-side.

Surfaces: **Dashboard** (kb health/throughput), **Accounts** (client enrollment,
certificate revocation, scopes, OIDC config), **Governance** (decision records,
the policy-verdict action audit, the curator review queue). This document is the
trust model; the surfaces are built slice by slice (see
`docs/proposals/pending/kb-web-console.md` and its `.plan.md`).

## Status

**Default-off.** The console is a separate opt-in binary that only runs when
launched with a console-admin credential file, and binds to `127.0.0.1` unless
told otherwise. It is **not** wired into `compose.yaml`/the kb image until the S6
close-out (and even then it stays opt-in behind a compose profile). S0 ships the
service scaffold + containment model; the Dashboard/Accounts/Governance pages are
placeholders filled in later slices.

## Trust model

The console is a **privilege-escalation surface** and is treated as fail-closed,
not merely "semi-trusted".

- **Scoped console-admin credential, not owner.** The console holds a
  `scope:console-admin:<id>:<secret>` bearer whose route allowlist the **kb
  enforces server-side** (`src/kb/http/kb_route_acl.c`): only
  `/v1/console/overview`, `/v1/enrollments` (+ revoke), `/v1/config/oidc`,
  `/v1/scopes`, `/v1/decisions` (+ sub-actions), and `/v1/audit/actions`. A
  compromised console is bounded to that allowlist — never full-KB takeover (no
  arbitrary enroll minting, no owner routes). `/v1/review` is **not** in the set:
  review accept/reject is a separate `curator` scope (S4/S5).
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
