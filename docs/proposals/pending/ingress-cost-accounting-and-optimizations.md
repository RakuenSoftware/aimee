# Proposal: ingress cost accounting + request-level cost optimizations

- **State:** draft - pending review
- **Author:** JBailes
- **Date:** 2026-06-11
- **Charter roles:** Evaluate-Optimize (monetary reward signal feeding the
  existing bandit surface), Calibrate (cache/thinking thresholds), Recall
  (envelope cache-marking on the answer path), Gate-Promote (default-off flag
  rollout per the readiness program).
- **Scope:** `src/server/anthropic_http.c` (usage capture at the buffered +
  streaming ingress and `count_tokens`), `src/server/anthropic_stream*.c`
  (surface the provider's final `usage` block from `message_delta`),
  `src/server/ingress_preinject.c` (`ingress_preinject_format_envelope` /
  `ingress_preinject_apply` cache-marking), `src/server/delegate_economics.c`
  (promote the qualitative cost model to a monetary one), a new
  `src/server/cost_pricing.{c,h}` (pricing table + `cost_calc`), a new db2 ledger
  (`src/db2/usage_ledger.c` + header) reusing the `agent_outcomes` / `bandit`
  table idiom, a typed `/v1/usage/*` surface (`server_http_routes.inc` +
  handler), `aimee usage` CLI, the bandit reward loop in
  `src/server/server_compute.c` (attach a $ reward to the existing
  `delegate_routing` decision point), and config plumbing
  (`src/headers/config.h`, `src/config_fields.c`, `src/config_sections.c`,
  `src/config_save.c`). Unit + integration tests. No new service, no new model.

## Goal

Give aimee **native USD cost accounting** for every request that crosses its
ingress, and a small set of **request-level cost optimizations** that feed the
optimization surface aimee already has. aimee sits directly in the request path —
the Anthropic ingress (`POST /v1/messages`), the Codex/Responses `/v1` ingress,
and the delegate fan-out all flow through aimee-server — so it owns the exact
interception point that an external proxy (e.g. the `tokencost` project this is
modelled on) has to bolt on. Today aimee tracks token *counts* and a
*qualitative* cost tier; it does not know what a turn costs in dollars, cannot
attribute spend by source, and applies none of the cheap request-shaping wins
(prompt-cache marking of the stable context envelope, dedup, thinking-budget
caps).

This is deliberately **not** a new optimizer. aimee already has a learning
optimization surface — `delegate_routing` is a registered bandit decision point
with a closed reward loop (`docs/proposals/done/optimization-surface.md`). The
missing piece is a **monetary reward signal** and the **observability** to
compute it. This proposal supplies the dollars; the existing bandit consumes
them.

## §0 What already exists (so we don't rebuild it)

- **Token counts are already captured at the ingress.** `anthropic_http.c`
  proxies the raw provider call buffered (`messages`) and streaming
  (`messages_stream`), and exposes `count_tokens`. The streaming path runs an
  `anthropic_stream_xlate_t` translator that already parses provider SSE
  (`message_start` / `message_delta`) — the provider's authoritative `usage`
  (input/output/cache tokens) passes through it. We surface it rather than
  re-count.
- **Cache-token fields already flow through delegate economics.**
  `delegate_economics.c` already emits `delegate_cache_read_tokens` /
  `delegate_cache_write_tokens` and an `agent_cost_tier`, with a *qualitative*
  `delegate_cost_model` ("free" vs "tiered"). We promote this to dollars; we do
  not invent the token plumbing.
- **The optimization/routing surface is built and closed-loop.**
  `delegate_routing` is a sampled bandit decision point with a reward loop
  reaching `db2_bandit_decision_close()`, and `aimee optimize
  points|baseline|replay|run|compare|promote` exists. We attach a reward, not a
  framework.
- **The context envelope is a single, stable, cache-shaped block.**
  `ingress_preinject_format_envelope` builds `<aimee-context confidence=…>…`
  and `ingress_preinject_apply` injects it ahead of the instructions. It is large
  and constant across a session — the ideal prompt-cache anchor.
- **Persistence is Postgres (db2), not SQLite.** The ledger follows the
  `agent_outcomes` / `bandit` table idiom in `src/db2/`, not tokencost's
  `tracker.db`.

## §1 Pricing table + `cost_calc` (item 1)

New `src/server/cost_pricing.{c,h}`: a static, data-driven price map keyed by
model id, plus a single cache-aware cost function. Mirrors the economics the
provider actually bills:

```
cost = ( input_tok       * p.input
       + output_tok      * p.output
       + cache_read_tok   * p.input * 0.10     /* cached reads ~10% of input */
       + cache_write_tok  * p.input * 1.25 )   /* cache creation ~125% input */
     / 1e6;               /* prices are per-million tokens */
```

- Each entry: `{ input, output }` per-million USD. Claude (Opus/Sonnet/Haiku +
  the `[1m]` long-context tiers), the three delegates (minimax, mimo-2.5,
  mistral) whose limits aimee already records, plus the OpenAI-compatible
  providers aimee can target. A `"default"` fallback so an unknown model degrades
  to an estimate, never a crash.
- **Maintenance is the real cost here.** Prices drift, so the table ships as
  *data* with a dated `pricing_refreshed` marker and a single edit point; no
  prices baked into call sites. `tokencost` is MIT, so its ~218-model map can
  seed ours **with attribution** — but we only carry models aimee can actually
  reach.
- `cost_pricing_lookup(model)` and `cost_calc(model, in, out, cr, cw)` are pure
  and unit-testable with no I/O.

## §2 Request cost-tracking ledger (item 2)

A db2 ledger that records one row per ingress turn, written from the points that
already see the usage.

- **Schema** (`src/db2/usage_ledger.c`), columns chosen from what the ingress
  already has: `ts, source, model, input_tokens, output_tokens,
  cache_read_tokens, cache_creation_tokens, cost_usd, duration_ms, stop_reason,
  tool_call_count, optimizations_json, optimizer_savings_usd`. `source` is the
  key field — it separates Claude Code vs Codex vs webchat vs a named delegate,
  derived from the ingress entrypoint (not User-Agent sniffing; aimee knows its
  own caller).
- **Write points:** the buffered `messages` return and the `messages_stream`
  finish in `anthropic_http.c` (after `anthropic_stream_finish`, where the
  translator's final `usage` is known), and the delegate-result path that already
  populates `delegate_cache_*` fields. Estimated counts
  (`session_compact_estimate_tokens`) are only a fallback when the provider
  omitted `usage`; the row records which it was.
- **Read surface:** typed `/v1/usage/summary?period=…` and `/v1/usage/raw?limit=…`
  routes (registered in `server_http_routes.inc`, conformance-scanner clean — no
  stray `/v1` string literals per the scanner trap), plus `aimee usage
  summary|raw` over the thin client. This is the "what did this cost, by source
  and model" surface aimee lacks.

## §3 Auto prompt-cache marking of the context envelope (item 3) — highest leverage

The single best optimization for aimee specifically. The pre-injection envelope
is large and stable within a session; marking it cacheable cuts its repeat-read
cost ~90% (the `cache_read = 0.10 × input` line in §1).

- In `ingress_preinject_apply` (or at the point the envelope is attached to the
  outbound provider request in `anthropic_http.c`), add Anthropic
  `cache_control: {type: "ephemeral"}` to the envelope/system block when it
  exceeds a configurable size floor (default ~1k chars, matching the tokencost
  heuristic).
- For OpenAI-compatible providers without explicit cache controls, this is a
  no-op — the block is simply sent normally.
- Savings are computed from the realized `cache_read_tokens` in §2 and recorded
  in `optimizer_savings_usd`, so the win is *measured*, not asserted.

## §4 Short-window request dedup (item 4)

Cheap guard against duplicate identical turns (double-submits, client retries).

- A bounded in-memory map of `SHA256(canonical request body) → (response,
  ts)` at the ingress, TTL ~5s. On hit within the window, return the cached
  response and record a `dedup` optimization row with the full turn cost as
  saved.
- Strictly opt-in and conservative: streaming responses and any request carrying
  tool-result state bypass dedup (replaying a tool turn must not be elided).

## §5 Complexity score → thinking-budget cap (item 5)

A deterministic 0–10 complexity score (message count, total content length, tool
presence — the tokencost heuristic) used to **cap** extended-thinking budget, not
to change routing (routing already belongs to the bandit, §6).

- Low (<4) → small thinking budget; medium (4–7) → moderate; high → uncapped.
  Auto-raise the cap when the turn shows tool-error signals.
- Lives next to the attention-guard/request-shaping path
  (`agent_request_shaping.c`). Default-off; when off, provider/user-supplied
  thinking settings pass through untouched.

## §6 Close the loop: monetary reward into the existing bandit (the point)

Rather than a parallel "smart router," feed the dollars from §1–§2 back into the
`delegate_routing` decision point that already exists. After a turn closes,
`server_compute.c` attaches `cost_usd` (and quality outcome) as the reward when
calling `db2_bandit_decision_close()`. The bandit then learns cheap-vs-capable
routing from real spend, and `aimee optimize compare` can report $ deltas. This
is what turns aimee's interception position into a *self-tuning* cost surface
instead of a static dashboard — the differentiator over an external proxy.

## Config (all default-off, flag-rollout-readiness program)

New bool/int fields plumbed through `config.h` / `config_fields.c` /
`config_sections.c` / `config_save.c`, each defaulting to inert so this lands
dark and graduates per the 6-criterion readiness bar:

- `usage_accounting_enabled` (master gate for §1–§2 recording)
- `ingress_cache_marking_enabled` + `ingress_cache_min_chars` (§3)
- `ingress_dedup_enabled` + `ingress_dedup_window_ms` (§4)
- `thinking_budget_cap_enabled` (§5)
- `cost_reward_enabled` (§6 — attach $ reward to the bandit)

Accounting (§1–§2) is pure observation and is the natural first flag to flip;
the request-mutating optimizations (§3–§5) stay off until each clears the bar
with a benchmark showing no quality regression.

## Testing

- **Unit:** `cost_calc` against known provider invoices (incl. cache-read/write
  multipliers and the `default` fallback); complexity scoring fixtures; dedup
  hash/TTL boundaries; envelope cache-mark gating on the size floor.
- **Integration:** drive the Anthropic ingress (buffered + streaming) against a
  stub provider returning a known `usage` block; assert one ledger row with the
  right `source`, tokens, and `cost_usd`, and that streaming reads `usage` from
  the translator finish, not the estimate. Assert §3 marks the envelope only
  above the floor and that `optimizer_savings_usd` reflects realized
  `cache_read_tokens`.
- **Conformance:** `/v1/usage/*` routes pass the api-conformance scanner (build
  URLs from the documented prefix; no stray `/v1` literals).
- **Bench:** an A/B on the bench corpus with §6 on vs off, reporting $ /
  correct-answer, gated as a user-run job (no autonomous prod deploy).

## Non-goals / what we are *not* taking from tokencost

- No external proxy, no menubar app, no `dashboard.html`, no `onbording.sh`,
  no log-scraping of other tools' files — aimee already owns the request path.
- No silent message-trimming or model-substitution behind the user's back;
  routing decisions stay in the bandit where they are learned and auditable.
- No SQLite `tracker.db`; persistence is db2.

## Attribution

Modelled on the MIT-licensed `tokencost` project (pricing-table shape, cache
multipliers, complexity heuristic, dedup idea). Any pricing data seeded from it
carries attribution.
