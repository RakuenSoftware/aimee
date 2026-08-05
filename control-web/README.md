# KB console

`control-web` is the browser administration client for a shared `aimee-kb`.

It owns browser login, sessions, CSRF, a deny-by-default proxy, and console-local action audit. It
does not own KB data or reuse runtime-web PAM authority.

| File | Responsibility |
| --- | --- |
| `auth.go` | OIDC verification and explicit break-glass path |
| `session.go` | per-user SQLite sessions and CSRF token |
| `acl.go` | shared control-web module route policy |
| `proxy.go` | authenticated `/api` to KB `/v1` proxy |
| `audit.go` | console administration audit |
| `tls.go` | HTTPS setup |

The physical proxy uses the same policy package as the separately supervised
control-web module. The KB obtains its authoritative console-admin decision from
that module over the event bus and has no duplicate local allowlist.

```bash
cd control-web
go test ./...
go build ./...
```

See [KB console](../docs/KB_CONSOLE.md).
