# Packet: Slice 0b — reconcile cost_tier with catalog price

**Depends on:** Slice 0 (catalog identity correctness) merged first. Do not start before it.

## Problem

`agent_route()` (`src/server/agent_config.c:1695`) minimizes `cost_tier`. That integer is
hand-entered per agent and currently contradicts published price. Live config vs. models.dev
($/Mtok input, fetched 2026-07-22):

| agent | model | live cost_tier | catalog price in/out | correct tier |
|---|---|---|---|---|
| `MiniMax-M3` | MiniMax-M3 | 0 | 0.30 / 1.20 | 0 |
| `kimi-k2.7-code` | kimi-k2.7-code | 0 | 0.95 / 4.00 | 1 |
| `claude` | claude-opus-4-8 | 1 | 5.00 / 25.00 | 3 |
| `codex` | gpt-5.6-sol | **0** | **5.00 / 30.00** | **3** |

`gpt-5.6-sol` is registered at the cheapest tier and is the most expensive model in the fleet —
output-pricier than Opus, which sits a tier above it. The router's "cheapest-first" selection is
therefore not minimizing cost.

Reference ladder from the catalog:

| tier | $/Mtok in | models |
|---|---|---|
| 0 | 0.30 | MiniMax-M3 |
| 1 | 0.95–1.00 | kimi-k2.7-code, claude-haiku-4-5, gpt-5.6-luna |
| 2 | 2.00–2.50 | claude-sonnet-5, gpt-5.6-terra |
| 3 | 5.00 | claude-opus-4-8, gpt-5.6-sol |
| 4 | 10.00 | claude-fable-5 |

## Required changes

1. A **lint/diagnostic** that compares each enabled agent's `cost_tier` against
   `cost_in_per_mtok` / `cost_out_per_mtok` from `model_capability_t` (already ingested by
   `capability_copy_from_json()`, `src/model_registry.c:678-690`) and fails when the tier
   ordering contradicts the catalog price ordering. Surface it via an existing diagnostic
   command (`aimee doctor` / `cmd_diagnose.c`) rather than a new top-level command.
2. Correct the live `cost_tier` values to match the ladder. This is a **config change**, so it
   must be proposed as an explicit operator action with the before/after table, not silently
   written by code.

## Explicitly NOT in scope

- Do NOT auto-derive `cost_tier` from price at load time. An operator must remain able to pin a
  tier (e.g. a flat-rate subscription where marginal token price is not the real cost). The
  lint reports the contradiction; it does not overwrite the operator.
- No routing-policy change. No competence axis. Do not enable
  `model_meta_capability_routing`.

## Known complication to handle explicitly

`codex` authenticates via `codex-oauth` against a ChatGPT subscription
(`endpoint: https://chatgpt.com/backend-api/codex`), so its **marginal** cost to this operator
may not be the per-token API price at all. The lint must therefore be a *warning with a
documented suppression*, not a hard failure — and the suppression must be recorded per agent
with a reason, so a genuinely mis-tiered agent cannot hide behind it.

This is the strongest argument against auto-derivation and should be reflected in the design.

## Acceptance criteria

- The lint flags `gpt-5.6-sol` at tier 0 against a $5.00/$30.00 catalog price.
- The lint passes once tiers match the ladder, or when an explicit per-agent suppression with a
  reason is present.
- A test covers: contradiction detected; suppression honored; missing catalog price does not
  crash or produce a false positive.
- No change to routing behavior from this packet alone.
