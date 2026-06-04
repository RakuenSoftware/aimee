# Claude Code ↔ aimee: Anthropic Messages ingress

**Status:** pending
**Charter role(s):** none (deterministic transport / integration surface — no
intelligence pass). Cites the Architecture Charter only for the service-topology
contract: this adds an inbound surface to `aimee-server`, no new store, no DB tier.

## Summary

Let any model aimee can already reach be used as the **primary model inside
Claude Code**. aimee-server gains an inbound **Anthropic Messages API** at
`POST /v1/messages` (plus streaming SSE and `POST /v1/messages/count_tokens`).
Claude Code is pointed at it with `ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN`;
every request is translated to aimee's currently-configured **primary agent**
(minimax, mistral, mimo, gemini, openai, or anthropic passthrough) and the reply
is translated back into Anthropic wire format.

```
Claude Code ──(Anthropic Messages API)──▶ aimee-server /v1/messages
                                              │  translate + route to primary agent
                                              ▼
                                          provider (minimax / mistral / … )
```

## Motivation

aimee already drives these providers as **delegates** (outbound), via the
`delegate_driver_t` vtable and the per-provider profiles. The one thing it can
*not* do today is let the operator's **own Claude Code session** run on those
models. Claude Code speaks exactly one wire protocol — the Anthropic Messages
API — and chooses its endpoint from `ANTHROPIC_BASE_URL`. So the missing piece
is purely an **inbound translator**: the inverse of the delegate drivers.

## Design

### A stateless wire-format proxy — *not* the agent loop

This is the load-bearing constraint. The proxy must **not** route through
aimee's chat/agent engine, memory assembly, persona, or toolset. Claude Code
constructs its own system prompt, message history, and tool definitions, and it
executes tools on its own side. aimee's job here is narrow: swap the model
transport and translate the wire format, passing `system` / `messages` / `tools`
through untouched (only re-encoded). Injecting aimee context here would corrupt
the context Claude Code carefully builds. aimee is a dumb gateway in this role.

### Surface (`aimee-server`)

| Route | Behaviour |
|-------|-----------|
| `POST /v1/messages` (`stream:false`) | Buffered; returns one Anthropic `message` object. |
| `POST /v1/messages` (`stream:true`)  | SSE: `message_start → content_block_start/delta*/stop → message_delta → message_stop`. Dispatched in `handle_conn` like `/v1/chat/stream`. |
| `POST /v1/messages/count_tokens`     | `{ "input_tokens": N }` from aimee's token estimator. |

Auth reuses the existing server bearer gate, additionally accepting the
Anthropic-style `x-api-key` header (Claude Code sends `ANTHROPIC_AUTH_TOKEN`
as `Authorization: Bearer`, or an API key as `x-api-key`). The rows live in the
declarative `server_http_routes.inc` registry so dispatch, the capability gate,
and the conformance/OpenAPI scan stay in sync.

### Model resolution

The inbound `model` string from Claude Code is **ignored**; the target is
aimee's current primary agent (per-session, else global), resolved through the
existing primary-agent lookup + agent registry. Switching models is
`aimee primary <agent>`. (A future extension could map the inbound model name to
a specific delegate; out of scope here.)

### Translation core (`server/anthropic_ingress.c`)

Pure, cJSON-only, fully unit-tested. The inverse of the outbound drivers:

- `anthropic_system_to_text` — flatten `system` (string or text-block array).
- `anthropic_messages_to_openai` — content blocks → OpenAI messages:
  `text`→text, `image`→`image_url` (base64 data URL), `tool_use`→assistant
  `tool_calls[]`, `tool_result`→`{role:"tool",tool_call_id,content}`.
- `anthropic_tools_to_openai` — `{name,description,input_schema}` →
  `{type:"function",function:{…,parameters}}`.
- `anthropic_response_from_parsed` — `parsed_response_t` → Anthropic `message`
  object (`content[]` of text/tool_use, `stop_reason`, `usage`).

For the **anthropic** provider driver the inbound body is already Anthropic-
shaped, so that path is near-passthrough (swap model/auth, strip unsupported
fields). The OpenAI-family providers use the conversions above; the existing
`agent_parse_response_openai` produces the `parsed_response_t`, and the SSE
branch reuses the OpenAI-chunk and `cli_claude.c` Anthropic-SSE parsers.

### Opt-in client wiring

`aimee claude-proxy enable|disable` (extending `client_integrations.c`) writes /
clears `ANTHROPIC_BASE_URL` + `ANTHROPIC_AUTH_TOKEN` under `env` in
`~/.claude/settings.json`. **Off by default**, because enabling it reroutes
*all* of the operator's Claude Code traffic — including the running session —
away from Anthropic to the primary delegate model. It must be explicit and
trivially reversible.

## Scope / phasing

1. Proposal + translation core + unit tests. *(this change)*
2. HTTP ingress: buffered `/v1/messages`, `count_tokens`, `x-api-key` auth.
3. Streaming SSE branch (required for real end-to-end use — Claude Code always
   streams `/v1/messages`).
4. Opt-in `claude-proxy enable/disable` + docs.

## Risks / non-goals

- **Fidelity:** tool-call round-trips must be exact or Claude Code's agent loop
  breaks; covered by unit tests on the block translation and by a live smoke.
- **Context budget:** Claude Code sends large contexts; the primary provider's
  context limit applies unchanged (no aimee-side compaction in proxy mode).
- **Non-goal:** aimee memory/persona/tool injection in this path. Non-goal:
  prompt caching translation (Anthropic `cache_control`) beyond pass-through.

## Verification

Unit tests for every translation function (text/image/tool_use/tool_result,
tools, response shaping, stop_reason, usage). End-to-end: point a real Claude
Code session at the ingress with primary = minimax and complete a tool-using
task. `aimee git verify` (full `-Werror` build + suite) before push.
