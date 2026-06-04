# aimee: VS Code extension

Chat with **aimee** inside VS Code. Prompts are routed through `aimee-server`'s
OpenAI-compatible `/v1` API, so they go through aimee's delegate fabric with
memory and guardrails applied, with no new server protocol, just the surface that
already ships (see [docs/VSCODE.md](../../docs/VSCODE.md) and
[the proposal](../../docs/proposals/pending/vscode-integration.md)).

> **Status: Tier 3 complete (phases 1-5).** Ships the `@aimee` chat participant
> (**stateful** - streams `/v1/responses` and threads `previous_response_id`, so
> aimee's session memory persists across turns), a server-health status item,
> command-palette queries (**aimee: Recall from memory** -> `/v1/memory/recall`,
> **aimee: Search knowledge base** -> `/v1/kb/search`, results in the *aimee*
> output channel), a **Language Model Chat Provider** registering "aimee" in the
> native model picker (select it as the model for any chat; streams
> `/v1/chat/completions`; requires VS Code 1.104+, the rest works on older
> builds), and a **docked chat panel** (**aimee: Open chat panel**). Reusing the
> full `frontend/` React webchat as the panel is a possible future refinement.

## Setup

1. Enable the aimee-server loopback `/v1` listener and get a token:

   ```bash
   aimee api status
   ```

   If it reports *disabled*, add to `~/.config/aimee/aimee.yaml`:

   ```yaml
   aimee:
     api:
       http_port: 8910
       bearer_token: "<generate-a-long-random-secret>"
       rate_limit_per_min: 60
   ```

   then `aimee server restart`. A `project:`-scoped bearer is recommended so the
   editor can read and chat but not perform admin mutations.

2. In VS Code settings, set:
   - `aimee.apiBase`: default `http://127.0.0.1:8910/v1`
   - `aimee.bearerToken`: your `bearer_token`

3. Open **Copilot Chat** and type `@aimee <your prompt>`.

The status-bar item (right side) shows whether aimee-server is reachable; click
it (or run **aimee: Show server status**) to re-check.

## Build (sideload)

```bash
cd editors/vscode
npm install
npm run compile        # tsc -> out/extension.js
# package a .vsix with: npx @vscode/vsce package
```

`npm run check` type-checks without emitting.
