# VS Code extension

The extension connects VS Code to `aimee-server` through existing `/v1` APIs. It adds:

- an `@aimee` chat participant;
- an aimee language-model provider in the model picker;
- memory and KB search commands;
- a server-health status item;
- a docked chat panel.

Conversation turns keep the server response/session identifier so memory and tool state continue
across turns. The extension owns no server state.

## Configure

Run:

```bash
aimee api status
```

Set the reported API base and an appropriately scoped credential in VS Code settings. Prefer an
enrolled HTTPS endpoint; use loopback HTTP only for a local server.

## Build

```bash
cd editors/vscode
npm install
npm run check
npm run compile
```

Package with `npx @vscode/vsce package` when the extension manifest and target VS Code version are
ready.

See [VS Code](../../docs/VSCODE.md).
