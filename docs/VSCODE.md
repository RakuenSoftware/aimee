# Using aimee with VS Code

aimee integrates with VS Code through surfaces that already ship in the
binaries, no extra protocol, no plugin build required. There are three ways to
wire it, and they are complementary:

| You want… | Use | What VS Code gets |
|---|---|---|
| aimee's **tools** (memory, guardrails, `find_symbol`, `delegate`) inside Copilot Chat | **MCP** (Path A) | aimee tools callable in agent mode |
| aimee itself **as the chat model** (delegate-routed, memory + guardrails) | **OpenAI `/v1` endpoint** (Path B) | "aimee" as a selectable model |
| aimee itself **as the chat agent** over stdio, no TCP listener or token | **ACP** (Path C) | "aimee" as an ACP agent |

For the deeper, native-extension experience (aimee in the model picker, an
`@aimee` chat participant, a docked webchat panel), a dedicated VS Code
extension lives in [`editors/vscode/`](../editors/vscode/), phase 1 ships the
`@aimee` chat participant + a server-health status item (sideload it per its
[README](../editors/vscode/README.md)). The two integration paths below also
work today without the extension.

> Requires a running `aimee-server` (`aimee status` to confirm) and `aimee` on
> your `PATH`. VS Code MCP support is GA in VS Code 1.102+.

---

## Path A, aimee tools via MCP (Copilot Chat agent mode)

VS Code reads MCP servers from a workspace `.vscode/mcp.json` or a user-level
`mcp.json`. Note VS Code uses the `servers` key (the CLI tools use
`mcpServers`, they are not interchangeable).

Create `.vscode/mcp.json` in your workspace:

```jsonc
{
  "servers": {
    "aimee": {
      "type": "stdio",
      "command": "aimee",
      "args": ["mcp-serve"]
    }
  }
}
```

Or register it globally via the Command Palette → **MCP: Open User
Configuration**, adding the same `aimee` entry.

Then open **Copilot Chat**, switch to **Agent** mode, and the aimee tools
(`search_memory`, `list_facts`, `find_symbol`, `delegate`,
`preview_blast_radius`, …) become available. Ask, e.g., *"search my aimee
memory for the database host"* and the model will call `search_memory`.

The full tool list is in [COMPATIBILITY.md](COMPATIBILITY.md#mcp-protocol).

---

## Path B, aimee as the chat model (OpenAI-compatible `/v1`)

aimee-server exposes a complete OpenAI-compatible API. Any VS Code extension
that accepts a custom OpenAI base URL, **Continue**, **Cline**, **Roo Code**,
or Copilot's own **Manage Models → OpenAI-compatible**, can use aimee as the
model. Requests are routed through aimee's delegate fabric with memory and
guardrails applied.

### 1. Enable the localhost TCP listener

The `/v1` surface is always served over the Unix socket; to reach it from a
VS Code extension you enable the optional loopback TCP listener. It refuses to
bind without a bearer token.

The quickest way is to let the CLI write the config and mint a token for you:

```bash
aimee api enable            # picks port 8910, generates a bearer, rate-limit 60
aimee api enable --vscode   # same, plus ready-to-paste VS Code / Continue / Cline snippets
```

`aimee api enable` persists the block below to `~/.config/aimee/aimee.yaml`
and prints the generated bearer token once. (`aimee api disable` turns the
listener back off.) Or edit the file by hand:

```yaml
aimee:
  api:
    http_port: 8910
    bearer_token: "<generate-a-long-random-secret>"
    rate_limit_per_min: 60
```

Either way, restart the server (`aimee server restart`) and verify:

```bash
curl -s http://127.0.0.1:8910/v1/models \
  -H "Authorization: Bearer <your-token>"
```

Or check the listener config and get ready-to-paste provider snippets without
touching `curl`:

```bash
aimee api status
```

`aimee api status` reports whether the loopback `/v1` listener is enabled, its
port, whether a bearer is configured (never the secret itself), and the
rate limit; when enabled it prints the per-extension base-URL / key / model
snippets below. When disabled it shows the `aimee.yaml` block to add.

The listener binds `127.0.0.1` only. Exposing it beyond loopback is the
operator's choice (reverse proxy) and always requires the bearer.

### 2. Point your extension at it

| Setting | Value |
|---|---|
| Base / API URL | `http://127.0.0.1:8910/v1` |
| API key | your `bearer_token` |
| Model | `aimee` |

**Continue** (`~/.continue/config.yaml`):

```yaml
models:
  - name: aimee
    provider: openai
    model: aimee
    apiBase: http://127.0.0.1:8910/v1
    apiKey: <your-token>
```

**Cline / Roo Code:** API Provider → *OpenAI Compatible* → Base URL
`http://127.0.0.1:8910/v1`, API Key = your token, Model ID = `aimee`.

### Endpoints available

`/v1/chat/completions` (streaming + non-streaming), `/v1/completions`,
`/v1/embeddings`, `/v1/responses` (stateful, continues an aimee session via
`previous_response_id`), and `/v1/runs` (async). See
[MANUAL.md §24](../MANUAL.md#24-the-http-v1-api) for the full list and auth
conventions.

> Tip: issue a **`project:`-scoped** bearer so the editor can read and chat but
> cannot perform admin mutations.

---

## Path C, aimee as an ACP agent (`aimee acp-serve`)

aimee speaks the [Agent Client Protocol](https://agentclientprotocol.com)
(ACP): newline-delimited JSON-RPC 2.0 over stdio. A VS Code extension that
speaks ACP launches `aimee acp-serve`, and aimee runs each turn on its primary
model with memory and guardrails applied, streaming the reply back as
`session/update` notifications. Like Path B this makes aimee the agent, but over
stdio instead of the OpenAI endpoint, so it needs no TCP listener and no bearer.

Point your ACP extension at the command:

```jsonc
{
  "command": "aimee",
  "args": ["acp-serve"]
}
```

aimee advertises its slash commands (`/help`, `/skill`, `/personality`,
`/compact`, `/reset`, `/stop`, `/queue`) in the ACP `initialize` handshake.
ACP turns run the agent against a local `aimee-server`; a network `/v1` remote
cannot serve them.

---

## Which path should I use?

- **Stack them.** Path A gives the model you already use (e.g. Copilot's
  GPT/Claude) access to aimee's memory and guardrails as tools. Path B and
  Path C both make aimee *itself* the agent (Path B over the OpenAI endpoint,
  Path C over ACP). Run Path B or C with aimee as the model **and** Path A so
  that model can also reach aimee's tools.
- For the tightest native experience (aimee in the model picker + `@aimee`
  participant + docked webchat), use the dedicated VS Code extension in
  [`editors/vscode/`](../editors/vscode/) (phase 1 ships the `@aimee` chat
  participant); Path A and Path B above also work without it.
