# Proposal: ingress cost-accounting coverage + request-level cost optimizations

- **State:** draft - pending review (revised after PR #180 review)
- **Author:** JBailes
- **Date:** 2026-06-11
- **Charter roles:** Evaluate-Optimize (cost-shaped reward into the existing
  bandit), Calibrate (cache/thinking thresholds), Recall (cache-aware ingress
  shaping only where the ingress already owns the prompt), Gate-Promote
  (default-off flag rollout per the readiness program).
- **Scope:** `src/server/anthropic_http.c` (observe provider usage in the native
  Anthropic relay path + buffered path; resolve the billable model),
  `src/server/anthropic_ingress.c` (`anthropic_stream_feed_openai` - capture full
  normalized OpenAI-stream usage, not just `completion_tokens`),
  `src/server/openai_chat.c` (the live OpenAI/Codex pre-injection seam),
  `src/server/agent_runtime.c` (`agent_log_call` - fix model attribution so rows
  resolve a real billable model), `src/server/token_tracker.c` +
  `src/headers/token_tracker.h` (single pricing source of truth, reconciled with
  the model registry), `src/headers/model_registry.h` (`cost_in_per_mtok` /
  `cost_out_per_mtok` - the authoritative base-price fields),
  `src/db1/token_audit.c` + `src/db1/schema.sql` (the existing audit ledger plus
  any required migration fields), `src/server/ingress_preinject.c` (envelope
  metadata/splitting only if needed by the OpenAI/Codex seam),
  `src/server_insights.inc` (the `insights.overview` implementation),
  `src/cmd_core.c` (`cmd_usage`), `src/server/server_compute.c` (cost-shaped
  reward on the existing `delegate_routing` bandit close), config plumbing
  (`src/headers/config.h`, `src/config_fields.c`, `src/config_sections.c`,
  `src/config_save.c`), route and auth registration only if a new API method is
  still justified. DB2 only if a shared/multi-machine analytics need is
  established (§2). Unit + integration tests. No new service, no new model.

## Revision note

This supersedes the first draft, which proposed new `cost_pricing.{c,h}`, a new
db2 usage ledger, a new `aimee usage` CLI, and raw-dollar bandit rewards. The PR
#180 review correctly showed that aimee **already ships** cache-aware pricing
(`token_tracker.c`, `token_estimate_cost`), an audit ledger with much of the
needed schema (`db1/token_audit.c`: `session_id, delegation_id, model,
prompt/completion/cache tokens, estimated_cost_usd`), a reader (`cmd_usage`
→ `db1_token_audit_*`), a usage/cost summary route (`/v1/insights/overview`), and
model-registry price fields (`cost_in_per_mtok` / `cost_out_per_mtok`). All
verified in-tree. The objective is therefore **coverage and correctness of the
existing machinery for ingress requests**, not new accounting. Every §1–§6 below
is rewritten accordingly.

This revision also folds in second-pass review findings: Anthropic
`/v1/messages` is intentionally a stateless proxy and is **not** a current
pre-injection surface; the shipped pre-injection seam is in `openai_chat.c`; the
`<aimee-context>` envelope is per-turn query-derived and therefore not
inherently a stable cache prefix; and the current `token_audit` schema lacks
several fields this proposal needs (`source`, `usage_kind`, requested-vs-served
model, duration/stop metadata, optimization metadata). Those are explicit work
items below rather than assumptions.

## Goal

Ingress requests are a cost blind spot. Normal agent and delegate calls are
audited (`agent_log_call` → `token_audit`), but requests arriving on aimee's
**Anthropic ingress** (`POST /v1/messages`) and **OpenAI-compatible ingress** are
not consistently folded in. This proposal (a) extends the existing audit to cover
ingress turns with a correct billable model and source, and (b) adds a small set
of request-level optimizations — cache-aware shaping first — wired into the
optimization surface aimee already has, not a new one.

## §0 What already exists, corrected

- **Pricing is already cache-aware and shared.** `token_usage_t`
  (`token_tracker.h`) carries `input/output/cache_write/cache_read`;
  `token_estimate_cost()` (`token_tracker.c`) applies per-MTok input/output +
  cache-read/write multipliers via an internal substring table. The **model registry**
  (`model_registry.h`) separately carries `cost_in_per_mtok` /
  `cost_out_per_mtok`. Two price sources already exist — §1 reconciles them rather
  than adding a third.
- **The audit ledger already covers the basic counters.** `db1/token_audit.c`
  inserts `(session_id, delegation_id, project_name, tool_name, role, model,
  prompt_tokens, completion_tokens, cache_write_tokens, cache_read_tokens,
  estimated_cost_usd)` and exposes totals / by-role / by-tool / by-model /
  by-delegation aggregations. `cmd_usage` (`cmd_core.c`) and `insights.overview`
  (`src/server_insights.inc`) already read it. This is DB1 (SQLite), local.
- **The audit ledger does not yet encode all semantics this proposal needs.** It
  has no explicit source/client, no usage-kind (`realized`, `estimated`,
  `avoided`), no requested-vs-served model, no duration/stop reason, and no
  optimization metadata. Do not smuggle all of that through `role`/`tool_name` if
  downstream dashboards need to distinguish it. §2 defines the minimum migration.
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
- **The live pre-injection surface is not Anthropic Messages ingress.**
  `docs/proposals/done/context-preinjection-ingress.md` states this explicitly:
  the Codex/OpenAI handlers inject the envelope, while Anthropic `/v1/messages`
  stays a pure stateless proxy by design. The live call sites are in
  `src/server/openai_chat.c`, with `ingress_preinject_build()` and sometimes
  `ingress_preinject_apply()`. Anthropic support would be a separate opt-in
  mutation phase, not a free cache-marking change in `anthropic_http.c`.
- **The `<aimee-context>` envelope is not inherently stable.**
  `ingress_preinject_build(query, ...)` rebuilds from the turn query, code
  search, memory context, and fresh audit context. Cache marking must separate
  stable prefix material from volatile per-turn retrieval material or prove
  stability by realized cache-read measurements. Do not mark the whole envelope
  as a stable cache anchor by assertion.
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
  model" contract; add a unit-tested registry fallback path. If substring
  matching remains as a fallback, test ambiguous names explicitly (`gpt-4o-mini`
  before `gpt-4o`, `o3-mini` before `o3`, etc.).
- Pin a dated `pricing_refreshed` marker in whichever file becomes authoritative
  so price drift is auditable.
- Treat provider-reported model aliases as normalization inputs. Cost lookup
  should use the billable provider model after alias resolution, not an agent
  nickname or a user-requested model string.

## §2 Cover ingress turns in the existing audit (token_audit), not a new ledger

Write ingress turns into `token_audit` from the points that can observe real
provider usage, distinguishing **billable realized usage** from **local
estimates** and **avoided estimated cost**.

- **Minimum schema migration:** add fields (or a tightly-linked extension table)
  for `source`, `usage_kind`, `requested_model`, `served_model` or
  `billable_model`, `duration_ms`, `stop_reason`, and `optimizations_json` /
  `avoided_cost_usd` if dedup/cache savings are reported. If the decision is to
  avoid migration, state exactly how each field is represented and which reports
  lose fidelity.
- **Native Anthropic relay path:** add a usage-observing tap to
  `anthropic_relay_chunk_cb` (or route native streams through a thin
  usage-observing relay) that parses `message_start` / `message_delta.usage`
  (input, output, `cache_read_input_tokens`, `cache_creation_input_tokens`) while
  still relaying the original bytes unchanged. Write one realized `token_audit`
  row at stream finish.
- **OpenAI-style streaming path:** request `stream_options: {include_usage:true}`
  only for providers known to support it; unsupported OpenAI-compatible providers
  must continue to work. Extend `anthropic_stream_feed_openai` state to hold full
  normalized `token_usage_t` (input + cache + output), not just
  `completion_tokens`. The provider request builder has to insert the option, not
  only the parser.
- **Buffered path:** parse the provider JSON `usage` block after a 200 response
  and before translating it back to the client. Use provider parser fields where
  already available, but verify cache tokens survive parser normalization.
- **`count_tokens`:** never writes a realized spend row. If exposed in usage
  summaries, it must be `usage_kind=estimated` and excluded from spend totals by
  default.
- **Failure:** no realized-spend row on provider error. If failed calls are
  reported at all, they are operational telemetry, not cost rows.
- **Source + model:** record source/client explicitly (Claude Code, Codex,
  webchat, OpenAI-compatible ingress, delegate) and resolve a real billable
  `model` (see §2a) instead of the empty string `agent_log_call` writes today.
- **DB1 vs DB2:** keep the per-row ledger in DB1 (local, where `cmd_usage` and
  `insights.overview` already read). Add a DB2 mirror/aggregate **only** if a
  specific shared/multi-machine optimization-analytics need is established; if so,
  the proposal must state the DB1→DB2 relationship explicitly so operators never
  face two competing cost ledgers. Default: DB1 only.
- **Double-counting guard:** delegate child spend is already folded back to the
  parent via `db1_token_audit_cost_for_delegation()` and `db1_cost_fold_record()`.
  Ingress accounting must state whether parent summaries include child spend,
  exclude it, or show both with de-duplication.

### §2a Billable-model resolution (shared fix)

Define the precedence for the audited model and apply it to both ingress rows and
the existing `agent_log_call` empty-model weakness: **provider-reported model
(response) > resolved/served model (the primary agent aimee actually routed to) >
requested model**. Record the requested model separately when it differs (Claude
Code asks for one model; aimee serves the configured primary), so attribution is
auditable rather than silently wrong. Agent names are not pricing keys.

## §3 Cache-aware request shaping, scoped to real ingress ownership

Cache marking is valuable, but it must match the ingress and provider shape.
Anthropic caching is not a text transform — `cache_control` must land on the
correct structured system/content block in outbound provider JSON. OpenAI-style
prompt caching is automatic/provider-specific and usually has no portable
request metadata.

- **OpenAI/Codex ingress first:** target the live pre-injection seam in
  `src/server/openai_chat.c`. Because the current `<aimee-context>` is per-turn
  query-derived, do not assume the whole envelope is cacheable. Either split the
  pre-injection output into stable and volatile parts, or place cache boundaries
  on stable system/persona/history prefixes and leave volatile retrieval after
  the boundary.
- **Anthropic `/v1/messages` carve-out:** `anthropic_http.c` is explicitly a
  stateless proxy. Default behavior is to preserve client-supplied structured
  system blocks and any existing `cache_control` metadata. Adding Aimee-generated
  cache controls or context to this path requires a separate opt-in flag and
  tests proving it does not corrupt Claude Code-owned tools, messages, or system
  arrays.
- **Per provider shape:** Anthropic native `/v1/messages` — only emit
  `cache_control: {type:"ephemeral"}` on an owned structured block and only when
  the provider is Anthropic. OpenAI-compatible — no-op or provider-specific
  automatic caching; never emit Anthropic cache metadata. Translated
  OpenAI-via-Anthropic vs non-Anthropic driver paths must be handled explicitly.
- **Negative tests required:** prove cache metadata is never leaked to an
  unsupported provider's request body; prove existing client-supplied
  `cache_control` blocks are preserved; prove Anthropic system arrays are not
  flattened when a structured mutation is needed.
- Savings are computed from realized `cache_read_tokens` (§2), not asserted.

## §4 Short-window dedup, narrowly scoped for v1

A response cache keyed only on `SHA256(body)` is unsafe. v1 is deliberately
narrow.

- **Key includes every behavior-affecting input:** provider/agent identity,
  resolved model, endpoint/provider, behavior-relevant config flags, the
  exact preinject/context identity, behavior-affecting request headers, auth
  principal or tenant boundary, and stream/non-stream mode.
- **v1 eligibility:** buffered only, no tools / no server-side tools, non-stream,
  successful `200` only, explicit idempotency key, and no request surface that can
  consume changing memory/context unless that context hash is in the key. Anything
  that can emit tool calls, consume changing memory/context, or depend on
  time/session state is excluded unless replay semantics are proven.
- **Window:** small TTL (~5s), bounded map, with per-source/account isolation.
- **Savings:** record `usage_kind=avoided` with *avoided estimated cost*, not
  "full turn cost saved," unless the skipped provider call's price is
  deterministically known from a prior realized row. Avoided cost must not be
  added to spend totals.

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
- **Provider support:** request mutation must be driver/capability-aware. Do not
  send unknown reasoning fields to OpenAI-compatible providers that reject them.
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
- **Accounting dependency:** shaped rewards must use realized child/delegate cost
  after cost-fold reconciliation, not an estimate based on agent name. If no
  realized cost exists, fall back to unchanged success reward and record why.

## §7 API surface: extend insights.overview before adding /v1/usage/*

`/v1/insights/overview` (`insights.overview`, implemented in
`src/server_insights.inc`) already reports usage/cost summaries. Default to
**extending it** with ingress/source breakdowns rather than adding `/v1/usage/*`.
If a separate route is still justified, the proposal must say why it does not
overlap `insights.overview` and keep the two non-overlapping.

Any new route requires:

- OpenAPI generator/source updates and regenerated route metadata;
- `server_http_routes.inc` registration that stays scanner-clean;
- CLI RPC route/client updates if the thin client exposes it;
- `server_auth.c` capability policy, not just route registration; and
- tests for auth denial as well as route/spec parity.

## Config (all default-off, flag-rollout-readiness program)

- `ingress_usage_accounting_enabled` (§2 — observe + audit ingress turns)
- `ingress_cache_marking_enabled` + `ingress_cache_min_chars` (§3)
- `anthropic_ingress_cache_mutation_enabled` (§3 carve-out; default off, likely
  later phase)
- `ingress_dedup_enabled` + `ingress_dedup_window_ms` (§4)
- `reasoning_effort_cap_enabled` (§5)
- `cost_reward_lambda` + `cost_reward_enabled` (§6; 0 / off ⇒ pure side-metadata)

Accounting (§2) is pure observation and is the first flag to flip; schema/report
changes should land before request mutation. The request-mutating optimizations
(§3–§5) and the shaped reward (§6) stay off until each clears the readiness bar
with a benchmark showing no quality regression.

## Testing

- **Pricing:** unknown-model → 0.0; cache-read/write fields applied; registry
  lookup/fallback; ambiguous substring ordering; provider alias normalization.
- **Schema/reporting:** migration preserves existing `token_audit` rows; spend
  totals exclude `estimated` and `avoided` rows by default; source/model
  breakdowns are stable in `cmd_usage` and `insights.overview`.
- **Buffered ingress:** provider usage writes **exactly one** audit row with
  resolved source + billable model; **no row** on provider failure.
- **Native Anthropic streaming:** final `message_delta.usage` is observed while
  the original stream is relayed byte-for-byte unchanged.
- **OpenAI-style streaming:** with `stream_options.include_usage` enabled only for
  supporting providers, full normalized usage (input + cache + output) is
  captured, not just `completion_tokens`; unsupported providers do not receive
  the option.
- **count_tokens:** asserts **no** spend ledger row.
- **Prompt-cache/request shaping:** OpenAI/Codex seam handles stable-vs-volatile
  preinject placement; Anthropic structured-JSON insertion only under the
  explicit opt-in carve-out; below-floor no-op; unsupported-provider no-op; no
  cache-metadata leakage; existing client cache controls preserved.
- **Dedup:** TTL; per-source/account/agent/model/endpoint/flags/context/stream
  key separation; no-tools/no-stream/200-only/idempotency-key eligibility;
  error-response bypass; avoided cost does not inflate spend totals.
- **Bandit reward:** reward stays in `[0,1]`; cost affects arm selection only
  through normalized shaping, never raw dollars; side-metadata mode leaves arm
  selection unchanged; missing realized cost falls back safely.
- **Model attribution:** ingress and `agent_log_call` rows resolve a non-empty
  billable model per §2a precedence; requested-vs-served divergence recorded.
- **API/auth:** any new usage route has route/spec parity, CLI RPC coverage where
  exposed, and `server_auth.c` capability tests. If only `insights.overview` is
  extended, test the existing route and capability.
- **Bench:** A/B on the bench corpus, §6 on vs off, reporting $/correct-answer;
  user-run job (no autonomous prod deploy).

## Non-goals

- No new pricing module, no new usage ledger, no new `aimee usage` command —
  unless `cmd_usage` and `insights.overview` are proven insufficient; extend
  `token_tracker` / model registry / `token_audit` / `insights.overview` /
  `cmd_usage` first.
- No SQLite `tracker.db`; per-row ledger stays in DB1, DB2 only if shared
  analytics is justified (§2).
- No silent message-trimming or model substitution; routing stays in the bandit.
- No mutation of Anthropic Messages ingress by default; it remains a stateless
  proxy unless a separate opt-in phase changes that contract.
- No external proxy, menu-bar widget, standalone dashboard, or log-scraping of
  other tools' files — aimee owns the request path.
