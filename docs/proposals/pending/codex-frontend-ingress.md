# Proposal: Aimee as a full-parity model backend for the Codex CLI/TUI

- **State:** implemented (P1–P3) — pending review
- **Author:** JBailes
- **Date:** 2026-06-04
- **Scope:** `src/server/openai_chat.c`, `src/server/openai_shape.c`,
  `src/server/agent_runtime.c` (+ `agent_protocol.h`), `src/server/server_http.c`,
  unit + integration tests, docs. New translation helpers; no new long-lived service.

## Status

**Implemented and validated end-to-end against the real Codex CLI 0.135.0.** A
scratch `aimee-server` (primary agent pointed at a mock OpenAI provider) driven by
the real `codex` binary completed the full two-turn tool loop: Codex sent the
`/v1/responses` turn, aimee emitted a `function_call`, Codex executed it locally,
returned `function_call_output`, and aimee's second turn rendered the final
answer — no `OutputTextDelta without active item` error. `/v1/models` returns the
Codex `{models:[…]}` schema from the registered primary agents.

**Critical fix beyond the original plan:** `server_http.c` capped request bodies
at **64 KB**. Codex turns are ~150–175 KB (instructions + 18 tool schemas + the
growing history), so they were truncated → JSON parse failed → the request fell to
the unary handler and 400'd. Raised the cap to **4 MB** (`SHTTP_MAX_BODY`). This
was the actual blocker for "point codex at aimee".

## Goal

**Full parity.** To the open-source Codex CLI/TUI, aimee must behave *exactly
like any other model Codex can use* — point Codex's model provider at aimee and
it is indistinguishable from `gpt-5.x`:

- Codex drives its normal agent loop against aimee's `/v1/responses`.
- aimee runs the turn on a **registered primary-agent model** (selected by the
  request `model` field).
- aimee participates in Codex's **tool loop**: it emits `function_call` items,
  **Codex executes them in the user's workspace/sandbox**, returns
  `function_call_output`, and the loop continues to a final answer.
- aimee serves **`/v1/models`** listing its registered primary-agent models, so
  Codex can **list and switch models** from its UI.

> Codex <-> aimee `/v1/responses` (transparent proxy) <-> primary agent's provider/model.
> Tools are passed through to the model; tool *execution* stays with Codex.

This is the Codex sibling of the in-flight Claude Code Anthropic `/v1/messages`
ingress — a stateless wire-format proxy onto aimee's primary agent. It is **not**
aimee running its own internal tool loop.

## User configuration (drop-in)

```toml
# ~/.codex/config.toml
[model_providers.aimee]
name = "aimee"
base_url = "http://<aimee-host>:<port>/v1"
wire_api = "responses"
env_key = "AIMEE_API_KEY"          # aimee loopback bearer
requires_openai_auth = false

model_provider = "aimee"
model = "aimee"                     # or any slug from GET /v1/models
```

## Verified wire contract (captured from Codex CLI 0.135.0)

All of the following was captured by pointing the real `codex` binary at a
logging/replay server — not from docs. **The full tool loop was reproduced
end-to-end** (Codex executed an aimee-issued `function_call` and returned its
output).

### A. Request: `POST /v1/responses`

- Headers: `accept: text/event-stream`, `authorization: Bearer <key>`,
  `content-type: application/json`, plus informational `x-codex-*` / `session-id`
  / `thread-id` headers.
- Body keys: `model`, `instructions` (~20 KB Codex persona), `input` (structured
  item array), `tools` (18 defs), `tool_choice`, `parallel_tool_calls`,
  `reasoning` (null | `{effort,summary}`), `store:false`, `stream:true`,
  `include:[]`, `prompt_cache_key`, `client_metadata`.
- `input` items:
  - `{type:"message", role:"developer"|"user"|"assistant", content:[{type:"input_text"|"output_text", text}]}`
  - `{type:"function_call", name, arguments, call_id}` (assistant's prior call, replayed each turn)
  - `{type:"function_call_output", call_id, output}` (tool result from Codex)
- `tools` are standard OpenAI **function** tools, e.g. `exec_command`:
  `{type:"function", name:"exec_command", parameters:{type:object, properties:{cmd:{type:string},...}, required:["cmd"]}}`.
  Codex also sends non-function tool *types* (`namespace`, `web_search`,
  `image_generation`) — see Non-goals.

Because `store:false`, **every turn carries the full transcript** (messages +
function_call + function_call_output). aimee is stateless per request.

### B. `GET /v1/models?client_version=…`

Codex caches this (`~/.codex/models_cache.json`). Expected shape is a **Codex-
proprietary** object (NOT the OpenAI `{object,data}` list):

```jsonc
{ "models": [ {
  "slug": "<model id used in the request `model` field>",
  "display_name": "...", "description": "...",
  "supported_in_api": true, "visibility": "list", "priority": 9,
  "context_window": 272000, "max_context_window": 272000,
  "effective_context_window_percent": 95,
  "default_reasoning_level": "medium",
  "supported_reasoning_levels": [{"effort":"low","description":"..."}, ...],
  "supports_reasoning_summaries": false, "default_reasoning_summary": "none",
  "shell_type": "shell_command", "apply_patch_tool_type": "freeform",
  "web_search_tool_type": "text_and_image", "supports_parallel_tool_calls": true,
  "supports_search_tool": false, "input_modalities": ["text"],
  "truncation_policy": {"mode":"tokens","limit":10000}
  /* ...full field set in models_cache.json... */
} ] }
```

aimee emits **one entry per registered primary-agent model**, `slug` = the id
Codex echoes in the request `model` field to switch models. When `/v1/models`
is absent/wrong Codex logs a cosmetic `missing field 'models'` and falls back to
the configured `model` — so this endpoint is required for the **picker UX**, not
for basic operation.

### C. SSE response — text item (minimal accepted sequence)

aimee's current stream emits only `created -> output_text.delta -> completed`,
which Codex **rejects** (`ERROR OutputTextDelta without active item`, all text
dropped). Verified minimal accepted sequence:

```
response.created
response.output_item.added   {item:{id, type:"message", status:"in_progress", role:"assistant", content:[]}}
response.output_text.delta   {item_id, output_index:0, content_index:0, delta}    (>=1)
response.output_item.done    {item:{id, type:"message", status:"completed", role:"assistant", content:[{type:"output_text", text:<full>, annotations:[]}]}}
response.completed           {response:{output:[<message item>], usage:{...}}}
```

`response.content_part.added` and `response.output_text.done` are NOT required.

### D. SSE response — function-call item (verified: Codex executed it)

```
response.created
response.output_item.added              {item:{id, type:"function_call", status:"in_progress", name, call_id, arguments:""}}
response.function_call_arguments.delta  {item_id, output_index:0, delta:"<args chunk>"}   (>=0)
response.function_call_arguments.done   {item_id, output_index:0, arguments:"<full args json>"}
response.output_item.done               {item:{id, type:"function_call", status:"completed", name, call_id, arguments:"<full>"}}
response.completed                       {response:{output:[<function_call item>], usage:{...}}}
```

Codex then runs the tool locally and POSTs the next turn with
`{type:"function_call", name, arguments, call_id}` + `{type:"function_call_output",
call_id, output}` in `input`. The `call_id` is the join key and must round-trip
verbatim.

SSE framing aimee already uses (`event: <name>\n` + `data: <json>\n\n`) matches.

## Design

aimee becomes a **transparent Codex-Responses <-> provider proxy**, routed
through the selected primary agent. It reuses the existing provider-agnostic
single-step layer in `agent_protocol.h` and does **not** run aimee's internal
tool loop.

Per `POST /v1/responses` turn:

1. **Parse** the Codex request into:
   - `messages`/`input` cJSON preserving structure: `message` items ->
     role+text; `function_call` -> an assistant tool-call message;
     `function_call_output` -> a tool-result message. (Upgrade over today's
     flatten-to-string parser.) Run `message_history_repair()` to heal orphaned
     calls/results, since each turn is stateless full-history.
   - `tools` cJSON: **pass Codex's `function` tools straight through** — they are
     already OpenAI function-tool shaped, which `agent_build_request_*` accepts.
     Optionally append aimee's own context/memory tools.
   - `system_prompt`: the primary agent's persona/system (optionally prefixed
     with Codex's `instructions`; default = aimee's own, per intent).
2. **Resolve the model**: `model` -> registered primary agent (see `/v1/models`).
   `""`/`"aimee"` -> session primary (`session_primary_get`) else `default_agent`.
3. **One provider step**: `agent_build_request_<wire>(agent, messages, tools,
   system_prompt, …)` -> POST to the provider -> `agent_parse_response_<wire>()`
   -> `parsed_response_t`. (Pick builder/parser by the agent's provider wire:
   openai / responses / anthropic / gemini.)
4. **Translate `parsed_response_t` -> Codex SSE**:
   - `content` (no tool call) -> text item events (§C).
   - `calls[]` -> one `function_call` item per call (§D), mapping
     `parsed_tool_call_t.id` <-> Codex `call_id`, `name`, `arguments`.
   - Always finish with `response.completed` carrying the same `output[]`.
5. Codex executes tools and loops; aimee handles each subsequent turn the same
   way (history now includes the tool results parsed in step 1).

### `/v1/models`

Replace/extend the current OpenAI-list provider with the Codex schema (§B),
generated from the registered primary-agent models (`agent_config_t`): `slug` =
agent/model id, `context_window`/reasoning/tool-type fields from the agent's
model metadata (sensible defaults where aimee has no equivalent). Keep the
OpenAI `{object,data}` shape available too if any other client needs it, but
Codex needs `{models:[…]}`.

### Components / changes

- `openai_shape.c`: new formatters `…_item_added` (message + function_call),
  `…_function_call_arguments_delta/done`, `…_item_done` (message + function_call);
  a Codex `/v1/models` builder. Declared in `openai_shape.h`.
- `openai_chat.c`: rework `responses_stream_handler` into the proxy/translator
  above; richer request parsing (structured input + tool history); model->primary
  resolution; non-stream `/v1/responses` kept for completeness.
- Reuse `agent_protocol.*` builders/parsers + `message_history_repair`.

## Phasing (independently shippable, same feature)

- **P1 — text renders:** §C item events + model->primary routing. Smallest; makes
  Codex usable as a chat frontend immediately. (Already prototyped/verified.)
- **P2 — model list + switch:** §B `/v1/models` from registered primary agents;
  honor `model` switching.
- **P3 — tool loop (the parity core):** §D function_call passthrough + structured
  request parsing + history repair. Delivers "exactly like any other model".

## Testing

- **Unit:** new formatters (message + function_call item shapes, args delta/done);
  request parser (message/function_call/function_call_output -> provider messages);
  `/v1/models` schema (required fields present, slug round-trips).
- **Integration (reproducible harness):** the capture/replay rig used to author
  this proposal — assert (1) text renders with no `OutputTextDelta without active
  item`; (2) an aimee-issued `function_call` is executed by Codex and the
  `function_call_output` round-trips by `call_id`; (3) `/v1/models` parses without
  the `missing field 'models'` warning.
- **Regression:** `/v1/chat/completions` (stream+unary) unchanged.

## Risks / open items

- **`parsed_tool_call_t.name[32]`** — Codex tool names fit (`exec_command`=12,
  `list_mcp_resource_templates`=27), but flattened `namespace`/MCP tool names
  (`mcp__codex_apps__github__*`) may exceed 31 chars. Widen the field or map
  names if we pass those through.
- **Special tool types** (`namespace`, `web_search`, `image_generation`) are not
  plain function tools; pass through only `function` tools in P3, drop/translate
  the rest (Non-goals).
- **`reasoning` summaries**: emit nothing in P3 (advertise
  `supports_reasoning_summaries:false`); a later pass can map provider reasoning
  to `response.reasoning_summary_text.*`.
- **Sandbox footnote:** on *this* dev box Codex's bubblewrap can't nest
  namespaces (container-in-container), so tool exec errors with `bwrap: Creating
  new namespace failed`. That is the host, not the protocol — the loop itself was
  verified working. Real user machines run it fine.

## Non-goals (this proposal)

- `web_search` / `image_generation` / `namespace` tool *types* and `view_image`
  multimodal input.
- Mapping provider reasoning to Codex `reasoning_summary` events.
- Durable/`store:true` response persistence (Codex uses `store:false`).
- Honoring Codex's 20 KB `instructions` persona by default (aimee answers as its
  primary agent; prefixing Codex's instructions is a config toggle).
