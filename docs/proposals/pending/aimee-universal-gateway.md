# Aimee as the universal LLM gateway: dual-API surface, model-derived proxy/translate, and a general inspect/alter pipeline

- **State:** reviewed — roundtable sign-off; **implementing.** P1 (derive
  proxy/translate, delete the `claude_proxy_parity` flag) is complete (#658); the
  remaining work is the `gateway_pipeline` core module + tool-policing (P2),
  memory-as-a-stage (P3), and delegate unification (P4). (R1
  surfaced 3 findings; R2: Findings A
  & B confirmed RESOLVED by security · architect · qa; Finding C resolved via the
  model-pin decision below). User proposal-gate decisions folded in. *Note: the
  delegate transcripts were truncated by aimee's citation-replay verification
  (stale code index on the review host); verdicts were front-loaded and captured.*
  One non-blocking item carried to the plan: spell out the OpenAI streaming
  block-buffering mechanism (criterion 3 demands it; only the Anthropic path is
  detailed here).
- **Scope:** the inbound model ingresses (`/v1/messages`, `/v1/chat/completions`,
  `/v1/completions`, `/v1/responses`) and the outbound provider call path
  (primary + delegate drivers). Intelligence-surface adjacent (it is the seam
  memory and guardrails attach to) but the proposal itself is plumbing.
- **Author:** JBailes (drafted by the engineer agent, 2026-06-23).
- **Origin:** follow-up to the `claude_proxy_parity` work (#648/#651 shipped a
  config flag to `main`; #654 to flip its default was closed). That flag was the
  **wrong model** — it made "passthrough vs. translate" a human-set toggle when it
  is really a derived property of the serving model. This proposal replaces the
  flag with the correct architecture and names the layer the flag was groping at.

## Thesis

**Aimee sits at the center of every model call. It is a translation layer, a
memory layer, and a guardrail/policy layer — and every call, from any client, to
any model (primary or delegate), passes through it.** The client's API and the
model's API are independent axes; aimee speaks the client's API on one side, the
model's API on the other, translating between them when they differ, and can
inspect and alter anything in between.

"Parity" is not a setting. It is the degenerate case where the client API equals
the model API and no transform is required — pure passthrough. Everything else is
a transform aimee applies.

## Problem — where aimee is today

Most of the surface already exists, which is why the right move is consolidation,
not green-field:

1. **Both client APIs are already served.** `/v1/messages` (Anthropic) and
   `/v1/chat/completions` + `/v1/completions` + `/v1/responses` (OpenAI) are live
   ingresses (`src/server/anthropic_http.c`, `src/server/openai_chat.c`).
2. **Model-side translation already exists.** Per-provider delegate drivers
   (`src/server/delegate_driver.c`, `delegate_openai.c`) own `build_request` /
   `parse_response`, so each speaks its model's native API. The Anthropic ingress
   already translates to OpenAI-family models via `anthropic_ingress.c`; the
   OpenAI ingress already reaches Anthropic models via the anthropic driver's
   `build_request`. The 2×2 (client-API × model-API) is largely wired.

The three real defects:

3. **Proxy/translate is bolted to a manual flag.** `claude_proxy_parity`
   (`src/headers/config.h`, gated in `anthropic_http.c` via `parity_on()`) layered
   a human toggle on top of a choice that is fully determined by the serving
   model's API (`driver_is_anthropic`). Wrong axis of control.
4. **The ingresses are deliberately stateless — calls do NOT go through aimee's
   systems.** `anthropic_http.c`'s own contract: *"does NOT run aimee's agent
   loop, memory, persona, or toolset."* The lone exception already bolted on is
   context pre-injection (`ingress_preinject_build`, wired ad-hoc into *both*
   ingresses) — proof that per-call alteration is wanted, but done as a one-off,
   not a general mechanism. Tool use still cannot be policed (e.g. prevent a model
   from spawning subagents), and full guardrails (`guardrails.h`, `guardrail_mode`)
   run only on the **agentic** path (`/v1/runs`) and the internal loop, not the
   proxy.
5. **No general interception point.** There is no single place where aimee
   normalizes a call and runs a defined sequence of inspect/transform stages.
   Translation is hand-coded per ingress; preinject is a one-off; guardrails are a
   separate subsystem. They should be stages of one pipeline.

## Target architecture

A single **gateway pipeline** every model call flows through:

```
client call (any API)
   │  normalize → canonical request IR
   ▼
[ request stages ]  inspect / alter the full request
   ├─ memory / context injection
   ├─ policy / guardrails (tool allow-deny, subagent policing, …)
   └─ (accounting, audit)
   │  render to the SERVING model's API  (passthrough if same API, else translate)
   ▼
provider call (primary OR delegate)
   │  normalize → canonical response IR
   ▼
[ response stages ]  inspect / alter the full response
   ├─ policy / guardrails (reject/rewrite disallowed tool_use, …)
   └─ (accounting, audit)
   │  render to the CLIENT's API
   ▼
client response (same API it called with)
```

Properties:

- **Two independent axes.** Client API ∈ {Anthropic, OpenAI}; model API ∈
  {Anthropic, OpenAI, …}. Passthrough on the diagonal, translate off it. The
  choice is **derived**, never configured.
- **General, not guardrail-specific.** A stage is a typed transform over the
  canonical IR that may read everything and rewrite anything (system, messages,
  tools, tool_choice, params; and on the way back, content/tool_use/usage). Tool
  policing (strip the subagent/`Task` tool from `tools`; reject a disallowed
  `tool_use`) is the *first* concrete policy, not a special case.
- **Bounded, not the agent loop.** The pipeline is per-call request/response
  transforms — it does not hijack the client's multi-turn loop or context
  ownership. This preserves the original reason the ingress was stateless ("don't
  corrupt the context the client builds") while still letting aimee inspect and
  alter each call. Memory injection stays cache-safe (append after the client's
  cached prefix, as today's preinject does).
- **Same path for primary and delegate.** A delegate call is just a gateway call
  whose serving model is the delegate. Aimee→model communication is already the
  driver layer; this unifies it with the inbound path so policy applies uniformly.

## Current state → target (gap analysis)

| Capability | Today | Target |
|---|---|---|
| Anthropic API in | ✅ `/v1/messages` | ✅ (through pipeline) |
| OpenAI API in | ✅ `/v1/chat/completions` etc. | ✅ (through pipeline) |
| Anthropic-in → Anthropic-model | ✅ passthrough | ✅ passthrough (derived) |
| Anthropic-in → OpenAI-model | ✅ translate | ✅ translate (derived) |
| OpenAI-in → OpenAI-model | ✅ passthrough | ✅ passthrough (derived) |
| OpenAI-in → Anthropic-model | ✅ via driver | ✅ translate (derived) |
| Proxy/translate selection | ❌ manual `claude_proxy_parity` | ✅ derived from serving model API |
| Inspect/alter proxied calls | ❌ stateless ingress | ✅ pipeline stages |
| Tool policing / prevent subagents | ❌ none | ✅ policy stage |
| Memory injection on proxy | ⚠️ ad-hoc `ingress_preinject_build` on **both** ingresses (anthropic_http.c ×2, openai_chat.c ×5) | ✅ one shared pipeline stage |
| Same path for delegates | ⚠️ driver layer, separate | ✅ unified |

## Phasing

- **P1 — derive, delete the flag. ✅ SHIPPED (#658, merged to `testing`).** Removed
  `claude_proxy_parity`; gates passthrough/translate purely on the serving model's
  API (`driver_is_anthropic`) in `anthropic_http.c`. Net behavior: an Anthropic call
  is forwarded untouched to an Anthropic model, translated to an OpenAI model —
  automatically. Covered by `src/tests/test_anthropic_http.c`
  (`messages_buffered_anthropic_parity_passthrough` honors the inbound model,
  `messages_buffered_openai_family_translates` + the streaming variant translate +
  swap). The single-model-shim behavior change (client model forwarded verbatim) is
  documented; the model-pin that bounds it lands in P2.
- **P2 — gateway pipeline scaffold + first policy.** Introduce the canonical
  request/response IR and a typed stage interface at the ingress seam; port the
  existing translation into it; add the first policy stage: **tool policing**
  (configurable tool allow/deny; the subagent/`Task` strip on request + disallowed
  `tool_use` rejection on response). Bounded, per-call, opt-in by policy.
- **P3 — memory as a stage.** Both ingresses already call
  `ingress_preinject_build` ad-hoc (anthropic_http.c, openai_chat.c). Fold those
  into **one** shared memory/context pipeline stage (DRY), cache-safe, identical
  rendered bytes.
- **P4 — unify delegates + symmetric coverage.** Route the OpenAI ingress and the
  delegate call path through the same pipeline so policy/memory/translation apply
  uniformly to every model call.

## Acceptance criteria

1. `claude_proxy_parity` is removed from the config surface
   (`config.h`/`config.c`/`config_save.c`/`config_fields.c`/`config_schema.inc`/docs);
   passthrough vs. translate on `/v1/messages` is decided solely by the serving
   model's API, verified by unit tests for both an Anthropic-model primary (model
   honored, betas forwarded, count_tokens proxied) and an OpenAI-model primary
   (translated, model swapped to the configured model).
2. A canonical request/response IR + stage interface exists with ≥1 translation
   stage and ≥1 policy stage, unit-tested in isolation (pure, no socket).
3. A tool-policing policy can, by config, strip a named tool (default target: the
   subagent/`Task` tool) from an inbound request's `tools` and police a matching
   `tool_use` in the response, on **both** the Anthropic and OpenAI ingresses,
   with an audit row per intervention. Response policing is implemented for the
   **buffered and streaming paths together** (not buffered-first); the action is
   **per-tool and per-severity** — e.g. a high-severity tool is blocked/rewritten
   even mid-SSE-stream, a low-severity one may be flagged/audited only.
4. The two ad-hoc `ingress_preinject_build` call paths (anthropic_http.c,
   openai_chat.c) are replaced by **one** shared pipeline stage with byte-identical
   rendered output, cache-safe (appended after the client's last `cache_control`
   breakpoint; `cache_read_input_tokens` unaffected on a repeated prefix).
5. Primary and delegate calls share the pipeline: a policy enabled in config
   applies to a delegate model call, demonstrated by a test.
6. No regression to the stateless guarantee that matters: the pipeline performs
   per-call transforms only and does not run the multi-turn agent loop; the
   client's cached prefix is preserved.

## Decisions (user proposal-gate, 2026-06-23)

- **Streaming + buffered policing together, per-tool/per-severity.** Response
  policing is implemented for both the buffered and streaming paths in the same
  phase — not buffered-first. The action is a function of the tool and the
  severity: a high-severity tool is blocked/rewritten even mid-SSE-stream; a
  low-severity one may be audited/flagged only.
  *Feasibility (resolves the security finding that mid-stream rewrite is
  impossible under today's byte relay):* when a tool-policing policy is active, the
  Anthropic streaming path stops using the raw byte relay
  (`anthropic_http.c` `relay_flush`/`relay_append_data`, which passes SSE bytes
  through unparsed) and instead drives a **block-aware translator** — the same
  `anthropic_stream_xlate` machinery already used for the OpenAI→Anthropic path.
  A `tool_use` content block streams its arguments as `input_json_delta`s and is
  fully bounded by `content_block_start`/`content_block_stop`; the translator
  **buffers the whole block before emitting `content_block_start` for it**, so the
  policy can drop or rewrite it before any byte reaches the client. Text blocks
  stream through unbuffered. So policing is feasible mid-stream **per content
  block** (not per arbitrary byte), at the cost of buffering one tool_use block;
  the byte relay remains the fast path when no policy is active.
- **The pipeline is a new CORE module `gateway_pipeline.{c,h}`** that the ingresses
  call — not grown into the size-capped `anthropic_http.c`/`openai_chat.c`.
- **Do not duplicate `/v1/runs`.** The agentic endpoint and this per-call pipeline
  **share** the policy/memory primitives (one implementation of tool-policing and
  memory injection, called from both); the pipeline must not re-implement them.

## Decisions, cont. — single-model Anthropic endpoints (resolves the qa finding)

P1 makes passthrough automatic for any Anthropic-API primary, which **honors the
client's requested model** (the parity path forwards `model` verbatim) instead of
swapping to the agent's configured `ag->model`. For a primary that is a
single-model Anthropic-*compatible* shim (llama.cpp / vLLM `/v1/messages`), an
arbitrary client model name would be forwarded and rejected upstream — a behavior
change vs. today's default (model-swap).

Resolution: **P1 honors the client model (the stated intent) and the P1 PR
documents the behavior change.** The fixed-model case is served by a **model-pin /
allowlist policy** — a P2 pipeline stage (config: pin to `ag->model`, or an
allowlist with swap-or-reject on miss). Default is honor; single-model operators
enable the pin. P1 keeps the existing fallback: when the client omits `model`, the
agent's model is used. This is a real but bounded regression window (P1→P2) for
the single-model-shim deployment class only; api.anthropic.com / multi-model
primaries are unaffected.

## Open questions (for roundtable)
- **IR fidelity.** A canonical IR must not lose provider-specific fields
  (thinking/signature blocks, cache_control, beta-gated features). The IR should be
  lossless for same-API passthrough (carry the raw blocks) and best-effort for
  cross-API translation, matching today's behavior.
- **Shared-seam location.** Where the shared policy/memory primitives live so both
  `/v1/runs` and `gateway_pipeline` call them without a layering violation
  (module-boundary check).
