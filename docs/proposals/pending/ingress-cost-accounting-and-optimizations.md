# Proposal: ingress cost-accounting coverage + request-level cost optimizations

- **State:** draft - pending review (revised after PR #180 review)
- **Author:** JBailes
- **Date:** 2026-06-11
- **Charter roles:** Evaluate-Optimize (cost-shaped reward into the existing
  bandit), Calibrate (cache/thinking thresholds), Recall (envelope cache-marking
  on the answer path), Gate-Promote (default-off flag rollout per the readiness
  program).
- **Scope:** `src/server/anthropic_http.c` (observe provider usage in the native
  Anthropic relay path + buffered path; resolve the billable model),
  `src/server/anthropic_ingress.c` (`anthropic_stream_feed_openai` — capture full
  normalized usage, not just `completion_tokens`; native SSE `message_delta.usage`
  observation), `src/server/agent_runtime.c` (`agent_log_call` — fix model
  attribution so ingress rows resolve a real billable model), `src/server/token_tracker.c`
  + `src/headers/token_tracker.h` (single pricing source of truth, reconciled with
  the model registry), `src/headers/model_registry.h` (`cost_in_per_mtok` /
  `cost_out_per_mtok` — the authoritative price fields), `src/db1/token_audit.c`
  (the existing audit ledger ingress rows feed), `src/server/ingress_preinject.c`
  (`ingress_preinject_build` / `ingress_preinject_format_envelope` — structured
  `cache_control` insertion), the `insights.overview` op behind
  `/v1/insights/overview`, `src/cmd_core.c` (`cmd_usage`), `src/server/server_compute.c`
  (cost-shaped reward on the existing `delegate_routing` bandit close), config
  plumbing (`src/headers/config.h`, `src/config_fields.c`, `src/config_sections.c`,
  `src/config_save.c`). DB2 only if a shared/multi-machine analytics need is
  established (§2). Unit + integration tests. No new service, no new model.

## Revision note

This supersedes the first draft, which proposed new `cost_pricing.{c,h}`, a new
db2 usage ledger, a new `aimee usage` CLI, and raw-dollar bandit rewards. The PR
#180 review correctly showed that aimee **already ships** cache-aware pricing
(`token_tracker.c`, `token_estimate_cost`), an audit ledger with the exact schema
I proposed (`db1/token_audit.c`: `session_id, delegation_id, model,
prompt/completion/cache tokens, estimated_cost_usd`), a reader (`cmd_usage`
→ `db1_token_audit_*`), a usage/cost summary route (`/v1/insights/overview`), and
model-registry price fields (`cost_in_per_mtok` / `cost_out_per_mtok`). All
verified in-tree. The objective is therefore **coverage and correctness of the
existing machinery for ingress requests**, not new accounting. Every §1–§6 below
is rewritten accordingly.

## Goal

Ingress requests are a cost blind spot. Normal agent and delegate calls are
audited (`agent_log_call` → `token_audit`), but requests arriving on aimee's
**Anthropic ingress** (`POST /v1/messages`) and **OpenAI-compatible ingress** are
not consistently folded in. This proposal (a) extends the existing audit to cover
ingress turns with a correct billable model and source, and (b) adds a small set
of request-level optimizations — prompt-cache marking of the stable injected
envelope first — wired into the optimization surface aimee already has, not a new
one.

## §0 What already exists, corrected

- **Pricing is already cache-aware and shared.** `token_usage_t`
  (`token_tracker.h`) carries `input/output/cache_write/cache_read`;
  `token_estimate_cost()` (`token_tracker.c`) applies per-MTok input/output +
  cache-read/write multipliers via an internal table. The **model registry**
  (`model_registry.h`) separately carries `cost_in_per_mtok` /
  `cost_out_per_mtok`. Two price sources already exist — §1 reconciles them rather
  than adding a third.
- **The audit ledger already has the proposed schema.** `db1/token_audit.c`
  inserts `(session_id, delegation_id, project_name, tool_name, role, model,
  prompt_tokens, completion_tokens, cache_write_tokens, cache_read_tokens,
  estimated_cost_usd)` and exposes totals / by-role / by-tool / by-delegation
  aggregations. `cmd_usage` (`cmd_core.c`) already reads them. This is DB1
  (SQLite), local.
- **The streaming translator does NOT see full Anthropic usage** (this was the
  draft's biggest error). `anthropic_http.c::messages_stream` has **two** paths:
  native Anthropic providers are relayed byte-for-byte through
  `anthropic_relay_chunk_cb` and **never** touch `anthropic_stream_xlate_t`; the
  translator is the OpenAI-style path only. And
  `anthropic_ingress.c::anthropic_stream_feed_openai` reads **only**
  `usage.completion_tokens` — no input, no cache tokens, no Anthropic
  `message_delta.usage`. So "the provider's authoritative usage already passes
  through the translator" was false. §2 must add the observation.
- **`count_tokens` is an estimate, not spend.** It uses
  `session_compact_estimate_tokens()` locally and never calls a provider. It must
  not write a spend row.
- **`ingress_preinject_apply` is not the live surface.** The implementation is
  `ingress_preinject_build()` + `ingress_preinject_format_envelope()`; `apply`
  exists only in the header/tests. §3 targets the real path.
- **The bandit reward is a scalar in [0,1].** `server_compute.c` closes
  `delegate_routing` with `rc == 0 ? 1.0 : 0.0`, and `kb_client_bandit_close`
  documents reward as `[0,1]` over beta-style posteriors. Raw dollars cannot be
  the reward (§6).
- **`agent_log_call` mis-attributes the model.** It estimates cost from
  `result->agent_name` and writes an empty `model` for those rows; and
  `anthropic_http.c` ignores Claude Code's *requested* model and routes to the
  configured primary agent. Billable-model resolution is its own work item.

## §1 One pricing source of truth (extend, don't add)

- **Pick one authority.** Either move the cache-aware multipliers into the model
  registry (add `cache_read_per_mtok` / `cache_write_per_mtok` beside
  `cost_in_per_mtok` / `cost_out_per_mtok`), or make `token_estimate_cost()`
  consume registry base prices and keep only the cache multipliers in
  `token_tracker`. Recommended: **registry is authoritative for base prices,
  `token_tracker` owns cache multipliers and the `token_usage_t` normalization** —
  one lookup, no third table.
- `token_estimate_cost()` keeps its current signature and the "0.0 on unknown
  model" contract; add a unit-tested registry fallback path.
- Pin a dated `pricing_refreshed` marker in whichever file becomes authoritative
  so price drift is auditable.

## §2 Cover ingress turns in the existing audit (token_audit), not a new ledger

Write ingress turns into `token_audit` from the points that can observe real
provider usage, distinguishing **billable** requests from **local estimates**.

- **Native Anthropic relay path:** add a usage-observing tap to
  `anthropic_relay_chunk_cb` (or route native streams through a thin
  usage-observing relay) that parses `message_start` / `message_delta.usage`
  (input, output, `cache_read_input_tokens`, `cache_creation_input_tokens`) while
  still relaying the original bytes unchanged. Write one `token_audit` row at
  stream finish.
- **OpenAI-style path:** request `include_usage`/`stream_options` where the
  provider supports it; extend `anthropic_stream_feed_openai` state to hold full
  normalized `token_usage_t` (input + cache, not just `completion_tokens`).
- **Buffered path:** parse the JSON `usage` block on the `messages` return.
- **`count_tokens`:** never writes a spend row — it is local estimation. Mark
  estimated-vs-realized so summaries never present pseudo-cost as spend.
- **Failure:** no row on provider error (no phantom spend).
- **Source + model:** set `role`/`tool_name` to carry the ingress source (Claude
  Code vs Codex vs webchat vs OpenAI-ingress) using fields the schema already has,
  and resolve a real billable `model` (see §2a) instead of the empty string
  `agent_log_call` writes today.
- **DB1 vs DB2:** keep the per-row ledger in DB1 (local, where `cmd_usage` and
  `insights.overview` already read). Add a DB2 mirror/aggregate **only** if a
  specific shared/multi-machine optimization-analytics need is established; if so,
  the proposal must state the DB1→DB2 relationship explicitly so operators never
  face two competing cost ledgers. Default: DB1 only.

### §2a Billable-model resolution (shared fix)

Define the precedence for the audited model and apply it to both ingress rows and
the existing `agent_log_call` empty-model weakness: **provider-reported model
(response) > resolved/served model (the primary agent aimee actually routed to) >
requested model**. Record the requested model separately when it differs (Claude
Code asks for one model; aimee serves the configured primary), so attribution is
auditable rather than silently wrong.

## §3 Structured prompt-cache marking of the injected envelope (highest leverage)

The pre-injection envelope is large and stable within a session; marking it
cacheable cuts its repeat-read cost (`cache_read ≈ 0.10 × input`). But Anthropic
caching is **not** a text transform — `cache_control` must land on the correct
**structured** system/content block in outbound provider JSON.

- Target the real surface: where `ingress_preinject_build` /
  `ingress_preinject_format_envelope` output is attached to the outbound provider
  request body in `anthropic_http.c`.
- **Per provider shape:** Anthropic native `/v1/messages` — set
  `cache_control: {type:"ephemeral"}` on the envelope's structured system/content
  block when it exceeds a configurable char floor. OpenAI-compatible — no-op (or
  provider-specific automatic caching) and **never** emit Anthropic cache
  metadata. Translated OpenAI-via-Anthropic vs non-Anthropic driver paths must be
  handled explicitly.
- **Negative test required:** prove cache metadata is never leaked to an
  unsupported provider's request body.
- Savings are computed from realized `cache_read_tokens` (§2), not asserted.

## §4 Short-window dedup, narrowly scoped for v1

A response cache keyed only on `SHA256(body)` is unsafe. v1 is deliberately
narrow.

- **Key includes every behavior-affecting input:** provider/agent identity,
  resolved model, endpoint/provider, behavior-relevant config flags, the
  preinject envelope identity, behavior-affecting request headers, and
  stream/non-stream mode.
- **v1 eligibility:** buffered only, no tools / no server-side tools, non-stream,
  successful `200` only, and an **explicit idempotency key** on the request.
  Anything that can emit tool calls, consume changing memory/context, or depend on
  time/session state is excluded unless replay semantics are proven.
- **Window:** small TTL (~5s), bounded map.
- **Savings:** record *avoided estimated cost*, not "full turn cost saved,"
  unless the skipped provider call's price is deterministically known.

## §5 Complexity score → reasoning-effort cap, mapped to real provider surfaces

A deterministic 0–10 complexity score (message count, content length, tool
presence) **caps** reasoning effort — it does not route (routing is the bandit,
§6).

- **Map to what exists:** aimee config exposes `model_reasoning_effort`, not a
  generalized numeric thinking budget. Anthropic native may use an
  extended-thinking budget object; OpenAI/Codex-compatible surfaces use a
  reasoning-effort enum; unsupported providers no-op.
- **Precedence:** an explicit user/provider setting is never overwritten by the
  cap unless a config flag opts into that. The cap only *lowers* effort on
  low-complexity turns (and may raise on tool-error signals).
- Lives in the request-shaping path; default-off.

## §6 Cost-shaped reward into the existing bandit (not raw dollars)

The `delegate_routing` close path takes a scalar reward in `[0,1]` over
beta-style posteriors. Feeding `cost_usd` directly would invert the semantics.

- **Reward shaping:** `reward = clamp01(quality_score − λ · normalized_cost)`,
  where `quality_score` is the existing success outcome (or a richer quality
  signal if available), `normalized_cost` is `cost_usd` normalized over a stated
  rolling window, and `λ` is a config weight. State the normalization window and
  the quality source explicitly.
- **Alternative (default first):** keep the reward scalar unchanged and store
  dollars as side metadata on the decision, consumed by `aimee optimize compare`
  for $-delta reporting without touching arm selection. Default to side-metadata
  first (lower risk), graduate to shaped reward behind a flag.

## §7 API surface: extend insights.overview before adding /v1/usage/*

`/v1/insights/overview` (`insights.overview`) already reports usage/cost
summaries. Default to **extending it** with ingress/source breakdowns rather than
adding `/v1/usage/*`. If a separate route is still justified, the proposal must
say why it does not overlap `insights.overview` and keep the two non-overlapping.
Any new route requires **OpenAPI generator/source updates** (the conformance
check verifies route/spec parity, not response-field completeness), plus
`server_http_routes.inc` registration that stays scanner-clean (no stray `/v1`
string literals).

## Config (all default-off, flag-rollout-readiness program)

- `ingress_usage_accounting_enabled` (§2 — observe + audit ingress turns)
- `ingress_cache_marking_enabled` + `ingress_cache_min_chars` (§3)
- `ingress_dedup_enabled` + `ingress_dedup_window_ms` (§4)
- `reasoning_effort_cap_enabled` (§5)
- `cost_reward_lambda` + `cost_reward_enabled` (§6; 0 / off ⇒ pure side-metadata)

Accounting (§2) is pure observation and is the first flag to flip; the
request-mutating optimizations (§3–§5) and the shaped reward (§6) stay off until
each clears the readiness bar with a benchmark showing no quality regression.

## Testing

- **Pricing:** unknown-model → 0.0; cache-read/write fields applied; registry
  fallback if the registry becomes authoritative.
- **Buffered ingress:** provider usage writes **exactly one** audit row with
  resolved source + billable model; **no row** on provider failure.
- **Native Anthropic streaming:** final `message_delta.usage` is observed while
  the original stream is relayed byte-for-byte unchanged.
- **OpenAI-style streaming:** with the usage chunk enabled, full normalized usage
  (input + cache + output) is captured, not just `completion_tokens`.
- **count_tokens:** asserts **no** spend ledger row.
- **Prompt-cache marking:** Anthropic structured-JSON insertion on the right
  block; below-floor no-op; unsupported-provider no-op; **no cache-metadata
  leakage** to unsupported providers.
- **Dedup:** TTL; per-agent/model/endpoint/flags/envelope/stream key separation;
  no-tools/no-stream/200-only/idempotency-key eligibility; error-response bypass.
- **Bandit reward:** reward stays in `[0,1]`; cost affects arm selection only
  through normalized shaping, never raw dollars; side-metadata mode leaves arm
  selection unchanged.
- **Model attribution:** ingress and `agent_log_call` rows resolve a non-empty
  billable model per §2a precedence; requested-vs-served divergence recorded.
- **Bench:** A/B on the bench corpus, §6 on vs off, reporting $/correct-answer;
  user-run job (no autonomous prod deploy).

## Non-goals

- No new pricing module, no new usage ledger, no new `aimee usage` command —
  extend `token_tracker` / model registry / `token_audit` / `insights.overview` /
  `cmd_usage`.
- No SQLite `tracker.db`; per-row ledger stays in DB1, DB2 only if shared
  analytics is justified (§2).
- No silent message-trimming or model substitution; routing stays in the bandit.
- No external proxy, menu-bar widget, standalone dashboard, or log-scraping of
  other tools' files — aimee owns the request path.
