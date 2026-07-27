# VS Code

aimee works with VS Code in three ways:

1. MCP tools for memory, code, and delegates;
2. ACP as an editor agent;
3. the aimee extension/model provider over `/v1`.

The browser deployment also offers a per-user code-server editor. That is separate from the native
extension.

## Native client setup

Run normal aimee client setup or register `aimee mcp-serve` in the VS Code user `mcp.json`. VS Code
uses a `servers` object. The local process inherits `remote.conf` and reaches the enrolled server.

For ACP, register `aimee acp-serve` where the editor supports it.

Use `aimee api status` for the model endpoint. Prefer an enrolled HTTPS URL and a narrowly scoped
credential.

## Extension

The extension adds an `@aimee` participant, an aimee model in the native picker, memory/KB search,
health status, and a docked panel. It keeps the server response/session identifier between turns;
conversation state remains on the server.

Build instructions are in [the extension README](../editors/vscode/README.md).

## Browser editor

The browser starts one code-server process per authenticated user and proxies it under the same web
origin. The selected project determines the workspace, but the server still checks user/project
authority. A code-server process has that user's source access and should be treated as an
interactive shell.

## Troubleshooting

- check `aimee remote status` from VS Code's environment;
- use an absolute `aimee` path if GUI PATH differs from the shell;
- check the MCP JSON key and stdio logs;
- verify endpoint TLS and credential scope;
- confirm the selected model supports tools if the client requests them;
- for browser editor failures, check per-user process state, project path, proxy auth, and port.
