# kb-console

Standalone Go thin-client that fronts a shared **aimee-kb**'s `/v1` for the web
console. See `docs/KB_CONSOLE.md` for the trust model and how to run it.

## What is (and is NOT) reused from `webchat/`

The console deliberately mirrors `aimee-webchat`'s *shape* but shares no code with
it, so the kb's auth surface is not coupled to webchat's PAM-typed upstream.

**Reused (as patterns, reimplemented here):**
- Auto-TLS HTTPS server with a self-signed cert (see `tls.go`).
- A SQLite session store (`session.go`), 0600, with a session cookie.
- An `/api/*` reverse proxy shape to the kb `/v1` (`proxy.go`).
- Serving a single inlined SmoothGUI SPA (`server.go` → `frontend/dist-console/`).

**NOT reused (intentionally):**
- `github.com/RakuenSoftware/smoothgui/auth` and `RunPAMHelper` — the console does
  no PAM. Login is OIDC (`auth.go`) with a presence-flag break-glass.
- Any `pam_*` import or webchat session/helper code.

## Files

| file | role |
|------|------|
| `main.go` | flags/env, config load, fail-fast startup probe, TLS, listen |
| `config.go` | config + 0600 cred-file contract + read-only OIDC file |
| `auth.go` | OIDC JWT verify (RS256/JWKS, pinned admin claim) + break-glass flag |
| `session.go` | SQLite sessions bound to `(iss,sub)`, timeouts, CSRF token |
| `acl.go` | Go mirror of the kb console-admin route allowlist (defence-in-depth) |
| `proxy.go` | deny-by-default `/api/*` → kb `/v1` with the console-admin bearer + CSRF |
| `server.go` | routes, strict CSP, login/logout/session/SPA handlers |
| `audit.go` | console-admin action audit (console-local sink) |
| `tls.go`, `helpers.go` | auto-TLS, small utilities |

## Build & test

```bash
go build ./...
go test ./...          # ACL, proxy deny-by-default, CSRF, session binding, break-glass-off
```

The kb-side containment lives in `src/kb/http/kb_route_acl.c` (unit-tested by
`src/tests/test_kb_route_acl.c`); the console's `acl.go` must stay in sync with it.
