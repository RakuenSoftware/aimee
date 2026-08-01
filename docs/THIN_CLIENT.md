# Thin client

`aimee` is a DB-free client for Linux, macOS, and Windows. It owns client-local work: hooks, stdio
protocols, source reads, uploads, and local CLI-agent execution. The server owns state, policy,
credentials, and model calls.

## Connect

```bash
aimee remote set https://host:8743 <bootstrap-bearer>
aimee remote status
```

The enrollment path pins the server certificate and rotates the bootstrap bearer. Linux also
enrolls a client mTLS certificate. Verify the printed fingerprint out of band.

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

Two checks apply:

1. deployment posture: `off`, `data`, or `full`;
2. the authenticated user's write grant.

`data` covers memory, document, and index writes. `full` also covers runner and workspace mutation.
Both checks must pass. A bearer that can read does not inherit write authority from a global config
value.

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
- A read-only grant returns `403` for writes.
- An unavailable client runner fails the local-CLI attempt; the server does not move that login to
  itself.
- A partial upload is not committed as a complete object.
- A server/client operation mismatch returns a typed unsupported-operation error.

Use `aimee remote status`, then `aimee status`, then the failing command with `--json` when available.
