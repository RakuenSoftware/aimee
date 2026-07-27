# KB console

`control-web` is the browser administration client for a shared `aimee-kb`.

It owns browser login, sessions, CSRF, a deny-by-default proxy, and console-local action audit. It
does not own KB data or reuse runtime-web PAM authority.

| File | Responsibility |
| --- | --- |
| `auth.go` | OIDC verification and explicit break-glass path |
| `session.go` | per-user SQLite sessions and CSRF token |
| `acl.go` | mirror of the KB console-admin route allowlist |
| `proxy.go` | authenticated `/api` to KB `/v1` proxy |
| `audit.go` | console administration audit |
| `tls.go` | HTTPS setup |

The console and KB allowlists must agree; the KB remains authoritative.

```bash
cd control-web
go test ./...
go build ./...
```

See [KB console](../docs/KB_CONSOLE.md).
