# Thin client

`aimee` is a DB-free client for Linux, macOS, and Windows. It owns client-local work: hooks, stdio
protocols, source reads, uploads, and local CLI-agent execution. The server owns state, policy,
credentials, and model calls.

## Connect

```bash
aimee remote set https://host:8743 <wizard-bearer>
aimee remote status
```

For the first user, copy this command from the setup wizard. The enrollment path pins the server
certificate and Linux generates/enrolls a client mTLS certificate. The server binds that certificate
to the authenticated wizard account with an explicit full grant. Verify the printed fingerprint out
of band.

For another client, copy the current primary bearer once, run `remote set`, then run
`aimee remote enroll` on that client. Enrollment mints an additional bearer and keeps every
existing client valid. `api.rotate_bearer` is deliberately different: it revokes the primary and
all enrolled bearers, so use it only for an explicit revoke-all operation.

Connection precedence is:

1. per-command transport flags;
2. `AIMEE_SERVER_URL` and token/certificate environment;
3. `~/.config/aimee/remote.conf`;
4. the local Unix socket.

`aimee remote clear` removes the persisted remote target and returns to the local socket.

## What stays on the client

- the checked-out working tree;
- local paths and filesystem handles;
- supported coding-tool hook and MCP/ACP configuration;
- native TLS trust state;
- installed provider CLIs and their local logins;
- runner execution explicitly delegated back to this client.

The client does not hold DB1, DB2, server vault keys, workflow state, or KB credentials.

## Workspaces and uploads

```bash
cd /path/to/project
aimee workspace add .
aimee index scan .
aimee kb docs push ./docs/design.pdf
```

The client reads and uploads bytes. The server records a detached workspace and sends content to the
KB. It never tries to open `/path/to/project` on its own filesystem.

Uploads are bounded and chunked where the operation permits it. A source change during an upload is
detected through size/hash metadata rather than accepted as one coherent file.

## Remote writes

The bearer is read-only by itself. The setup wizard's first user receives a certificate-bound `full`
grant when CSR enrollment completes; possession of only the bearer cannot exercise it. `full` covers
memory, document, index, agent, delegate, runner, and workspace-control operations.

Additional users use short-lived KB-signed identities and grants for the exact server, team, and
subject. `data` is sufficient for memory, document, and index writes. The old
`aimee.api.remote_writes` value is not an authorizer.

## Local CLI agents

Some providers are local programs rather than HTTP APIs. They need the local executable, terminal,
working tree, and login. When a remote workspace selects one, the server sends a bounded execution
request over the client runner channel.

The provider login stays on the client. The server receives output and status, not the credential.
Runner and workspace writes require full remote-write authority.

Claude CLI delegation is disabled by default. Enable it only after checking the provider's terms for
unattended use.

## Credentials

API keys, server-side OAuth tokens, git credentials, and delegate secrets belong in the server vault.
Do not keep them in `agents.json` or push them once per session.

Legacy client key files should be imported, verified, and removed using the installed agent/vault
commands. Keep a secure backup until the first successful provider probe.

## Client integrations

### Local model proxy

To route Codex through Aimee's model API, launch it with:

```bash
aimee launch --gateway -- codex
```

The thin client starts an authenticated listener on an unused `127.0.0.1` port,
selects the Aimee Responses provider in Codex, and disables the Aimee CLI/MCP
plugin for that invocation. It selects the server's `aimee` primary-agent binding;
use `codex -m <agent-name>` to select another binding advertised by `/v1/models`.
Your global Codex configuration is preserved. The listener lives until the launched
client exits; its exit status is returned by the launcher. Ordinary `aimee launch -- <client>` keeps
the client's own provider connection.

Codex 0.153.4 hook discovery ignores command-line plugin-disable overrides, so
the launcher creates a private `aimee-proxy-*.config.toml` profile in `CODEX_HOME`
(default `~/.codex`). It disables only `aimee@local`; other hooks and their trust
settings remain intact. The profile is removed on child exit, launch failure,
and handled termination. An uncatchable kill can leave this credential-free file
behind. The installed-plugin regression includes a positive control and verifies
that an unrelated user hook still executes.

This launcher targets Codex runtime commands, such as interactive Codex and
`codex exec`. An explicit Codex `--profile` after `--` is preserved, not consumed
as an Aimee remote profile. In that case, the launcher warns that your selected
profile must contain `[plugins."aimee@local"]` with `enabled = false` to suppress
the CLI hooks; no temporary profile replaces your selection.

The local credential is generated for each launch. Aimee replaces it with the
configured remote bearer and uses its existing server certificate pin and client
mTLS identity for the upstream connection. The remote bearer is not passed to
the launched client. Responses, including SSE and upstream HTTP errors, are
relayed as they arrive. Client disconnects close the upstream connection.

For a persistent listener managed by a terminal or service manager:

```bash
export AIMEE_PROXY_TOKEN="$(openssl rand -hex 32)"
aimee proxy --port 8911
```

Configure the consuming client with `http://127.0.0.1:8911/v1` and the same local
token. For Codex, use a custom provider with `wire_api = "responses"` and
`env_key = "AIMEE_PROXY_TOKEN"`; the variable must be available to both processes.
Disable the Aimee plugin when using this standalone setup to avoid also invoking
its tool integrations. The listener accepts Bearer authentication or Anthropic's
`x-api-key` header, permits only model API routes, and supports up to eight
concurrent requests. Requests need Content-Length framing and are limited to
4 MiB; HTTP upgrades, chunked request bodies, and browser-origin requests are
rejected. Streaming responses are not subject to that body-size limit. Remote
routes still have to be supported by the selected Aimee server.

A remote `401` or `403` is returned unchanged. If the server rejects the client
certificate, `aimee remote status` distinguishes that from a bearer rejection.
The server operator must check the enrollment/revocation state and provision a
valid identity; the proxy does not replace credentials or weaken TLS checks.

Run the regression suite with `make -C src proxy-tests`. It uses isolated local
HTTP/mTLS peers and, when installed, real Codex against deterministic Responses
fixtures, including failed streams. No model-provider account is needed. Set
`AIMEE_TEST_REQUIRE_CODEX=1` to fail if the compatibility client is missing.
The suite also runs with the native unit tests and sanitizers; CI and Linux
release-artifact validation require pinned Codex 0.153.4. The testing-artifact
publisher runs the transport suite before uploading its binaries.

### Tool integrations

Setup can register:

- Claude Code hooks and MCP;
- the Codex local plugin, hooks, and MCP;
- supported Copilot hooks and MCP;
- VS Code MCP/ACP/model endpoints;
- Claude Desktop MCP.

Set `AIMEE_NO_CLIENT_INTEGRATIONS=1` before setup to skip global changes. Registrations point to the
local `aimee` binary, which inherits the remote target.

## TLS by platform

| Platform | Backend | Automatic certificate enrollment |
| --- | --- | --- |
| Linux | OpenSSL | yes |
| macOS | Secure Transport | no; provision explicitly when required |
| Windows | Schannel | no; provision explicitly when required |

`AIMEE_TLS_INSECURE=1` is only for first contact with a known self-signed server. Confirm and pin the
fingerprint, then remove it.

## Failure behavior

- A changed certificate pin stops the connection.
- An expired or revoked client identity returns an authentication error.
- A missing, `off`, or insufficient grant returns `403` for writes.
- An unavailable client runner fails the local-CLI attempt; the server does not move that login to
  itself.
- A partial upload is not committed as a complete object.
- A server/client operation mismatch returns a typed unsupported-operation error.

Use `aimee remote status`, then `aimee status`, then the failing command with `--json` when available.
