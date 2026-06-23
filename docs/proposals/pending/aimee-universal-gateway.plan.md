# Implementation plan — Aimee universal gateway

- **State:** draft plan; tracks `aimee-universal-gateway.md` (roundtable-signed). Each
  packet is its own build → live-test → unit-tests → lint → roundtable → PR →
  merge cycle.

## P1 — derive proxy/translate; delete the flag

**Done (this branch).** Removes `claude_proxy_parity` from the whole config surface
(`config.h`, `config.c`, `config_save.c`, `config_fields.c`, `config_schema.inc`,
generated docs) and gates the `/v1/messages` ingress on `driver_is_anthropic` only:
Anthropic-API primary → honor inbound model + forward `anthropic-version`/
`anthropic-beta` + proxy `count_tokens` + relay upstream error status/body +
`Retry-After`; OpenAI primary → translate + swap model. Whitebox tests flipped
swap→honor. Live-validated against a mock upstream.

- **AC:** `claude_proxy_parity` gone; passthrough/translate decided solely by the
  serving model's API; unit tests green for both primary kinds; live test shows
  model honored on an Anthropic primary.
- **Behavior-change note for the PR:** single-model Anthropic-compatible shims now
  receive the client's model verbatim (model-pin lands in P2).

## P2 — `gateway_pipeline.{c,h}` + tool-policing

New CORE module `src/server/gateway_pipeline.{c,h}`. Canonical request/response IR
+ a typed stage interface (`apply_request`, `apply_response`, and a streaming
`on_block` variant). Port the existing translation in behind it; the ingresses call
the pipeline.

First policy: **tool-policing** (config: per-tool allow/deny + severity).
- Request side: strip a denied tool (default target: the subagent / `Task` tool)
  from `tools` (and `tool_choice` if it names it).
- Response side, **buffered**: drop/rewrite a denied `tool_use` block.
- Response side, **streaming** (both API shapes, per the carried-forward note):
  - **Anthropic SSE:** when a policy is active, drive the block-aware
    `anthropic_stream_xlate` instead of the byte relay; buffer a `tool_use` block
    (args = `input_json_delta`s bounded by `content_block_start/stop`) until
    `content_block_stop`, then emit/drop/rewrite. Text blocks stream unbuffered.
  - **OpenAI SSE:** a `tool_calls` delta carries `index` + incremental
    `function.arguments`; buffer per-index until the call is complete (next index,
    or `finish_reason`), then emit/drop/rewrite. Same per-block-not-per-byte rule.
  - Per-tool/severity: high-severity → buffer+block/rewrite; low-severity →
    pass-through + audit row.
- **Model-pin policy** (resolves proposal Finding C): config to pin the served
  model to `ag->model` or an allowlist (swap-or-reject on miss); default off.
- Shared with `/v1/runs`: the tool-policing + model-pin primitives live in one
  module both the pipeline and the agentic path call (no duplication).

- **AC:** IR + stages unit-tested pure; subagent strip + buffered & streaming
  response policing on both API shapes, audited; model-pin tested; a policy enabled
  in config applies via both ingresses.

## P3 — memory injection as a stage

Replace the two ad-hoc `ingress_preinject_build` call paths (anthropic_http.c ×2,
openai_chat.c ×5) with **one** shared pipeline memory stage; byte-identical,
cache-safe (append after the client's last `cache_control` breakpoint).

- **AC:** both ingresses route memory injection through the one stage; rendered
  bytes identical to today; `cache_read_input_tokens` unaffected on a repeated
  prefix; the old call sites are gone.

## P4 — unify OpenAI ingress + delegates

Route `openai_chat.c` and the delegate call path through the same pipeline so
translation + policy + memory apply uniformly to every model call, primary or
delegate.

- **AC:** a config-enabled policy applies to a delegate call (test); OpenAI ingress
  goes through the pipeline; no behavior change when no policy/memory is active.
