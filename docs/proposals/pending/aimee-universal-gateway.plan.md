# Implementation plan — Aimee universal gateway

- **State:** draft plan; tracks `aimee-universal-gateway.md` (roundtable-signed). Each
  packet is its own build → live-test → unit-tests → lint → roundtable → PR →
  merge cycle.

## P1 — derive proxy/translate; delete the flag

**✅ SHIPPED (#658, merged to `testing`).** Removes `claude_proxy_parity` from the whole config surface
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

**Split into verifiable slices** (a packet this size is not one safe PR):

- **P2a — request pipeline scaffold (✅ this PR).** New core module
  `src/gateway_pipeline.{c,h}` (NOT under `src/server/`, alongside the already-core
  `src/gateway_policy.c`, so the ingresses and `/v1/runs` share one seam). Canonical
  **request** IR `gw_request_t { cJSON *raw (BORROWED — stages mutate in place, never
  free); const void *driver, *ag (opaque, so the module never derefs server types —
  resolves the layering question); gw_api_t serving_api; int parity, stream }`, a
  typed stage `int(*)(gw_request_t*, void*)` returning an intervention count (≥0) or
  <0 (short-circuits), and `gw_pipeline_run_request()`. Two phases kept distinct:
  mutation stages run through the pipeline, then `translate_request` is the **terminal
  render** (it produces the provider shape rather than mutating `raw`, so stages
  always see the full untranslated request — lossless for same-API passthrough). The
  Anthropic ingress's two inline preludes (memory inject + tool policing, both paths)
  are routed through the pipeline; **zero behavior change** (the existing
  `test_anthropic_http` passthrough/translate suite still passes). Pure runner tests
  in `test_gateway_pipeline.c` (order, sum, short-circuit, null-safety). Request-side
  tool-policing itself (`gateway_policy_apply_request`) was already shipped; P2a only
  formalizes the seam it plugs into.
- **P2b — model-pin policy (✅ shipped #671).** Config-gated `gateway_pin_model`
  request stage, resolves Finding C. (The "buffered response policing" originally
  grouped here moves to P2c so response policing ships buffered+streaming together,
  per the user proposal-gate decision — never buffered-first.)
- **P2c — response-side tool-policing (buffered + streaming together).**
  Defense-in-depth backstop: drop/rewrite a disallowed `tool_use` the served model
  emits despite request-side stripping. Config-gated (`gateway_prevent_subagents`),
  default off.

  **Approach (roundtable-reviewed 2026-06-24, 0 blocking — chose B over A).**
  - *Why not the proposal's per-block streaming (A):* the Anthropic-native streaming
    path is a **raw byte relay** (`anthropic_relay_*`) with **no** SSE block parser,
    and `anthropic_stream_xlate` consumes only *OpenAI* chunks — so per-block policing
    of the Anthropic path needs a brand-new incremental Anthropic-SSE parser. High
    risk for a backstop control.
  - *Chosen (B) — buffer-when-active:* one policing implementation reused across all
    three response paths. When the policy is **active**, both streaming paths buffer
    the full upstream reply, police the assembled result (same logic as buffered),
    then **replay** it as a well-formed Anthropic SSE sequence. When the policy is
    **off (default)**, today's incremental relay/translator is unchanged (zero
    latency cost). Closes the `stream:true` bypass (streaming *is* policed when
    active); the only cost is loss of incremental delivery **while the backstop is
    on** — documented; per-block streaming is a follow-up optimization.

  **Implementation units.**
  1. `gateway_policy_is_denied_tool(name)` — predicate: policy-on ∧ subagent-tool
     (reuses `guardrails_canonical_tool_name`). Plus a centralized
     `police_parsed_response(parsed)` that drops denied `tool_call`s from
     `parsed_response_t.calls[]` and **recomputes `stop_reason`** (`tool_use` →
     `end_turn` when no calls remain). Audit row per drop.
  2. Buffered path (`messages_buffered`): police the parsed reply before
     `anthropic_response_from_parsed`.
  3. Streaming paths (`messages_stream`): when the policy is active, take the
     **buffered** upstream flow (non-stream call → parse → police) and emit via a new
     `emit_message_as_sse(message, emit, ctx)` replay helper that mirrors the
     `anthropic_stream_xlate` event shapes (message_start; per content block
     content_block_start/delta/stop; message_delta with recomputed stop_reason +
     **propagated usage** incl. cache_read; message_stop). Refactor the shared
     "get policed parsed reply" out of `messages_buffered` so both paths use one
     implementation.
  4. Tests: police drops denied calls + recomputes stop_reason; usage propagated;
     `emit_message_as_sse` produces a valid event sequence (incl. the all-tools-
     dropped → end_turn case); policy-off is byte-neutral on both paths.

### P2 design reference (carried into P2a/b/c)

New CORE module `src/gateway_pipeline.{c,h}`. Canonical request/response IR
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
