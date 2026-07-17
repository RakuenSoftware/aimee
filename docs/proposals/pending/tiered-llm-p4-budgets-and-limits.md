# Proposal: P4 — Budgets + rate limits (turn tracked spend into enforced caps)

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** P1 (teams), P2 (kb egress seam). Reuses P3's rollup if present but
  does not require it.

## Thesis

aimee *tracks* spend but never *caps* it, and its only rate limiter is a single
global 60-second bucket (`server_http_rate_check`, `src/server/server_http.c:282`)
— not per-team, not per-key. "Something that won't need a full-time engineer
babysitting it" means a POC team can't accidentally burn the month's budget on a
runaway loop. This packet adds enforcement at the one seam P2 created (kb egress),
where team identity and cost are both already in hand.

## Goal

Per-team (and optional per-project) **budget caps** and **rate limits** enforced at
kb egress: a call that would exceed a team's remaining budget or rate window is
refused with a clear, typed error before it reaches the vendor.

## §0 What already exists

- **Spend is computed per call** — `estimated_cost_usd` via `token_estimate_cost_ex`
  (P3 §0); at kb egress the team is resolved (P1) and the cost is known.
- **Cost caps exist for ensembles/roundtable** — `config.h:1805,1826`,
  `roundtable_preset.h:47` — a precedent for pre-flight cost gating, but scoped to
  internal orchestration, not per-team ingress.
- **Global rate limiter** — `server_http_rate_check` (one bucket) is the thing to
  *replace* with a keyed limiter, not extend.
- **Provider-side cooldown/rotation** — `delegate_credentials.c` handles upstream
  429s; unrelated to aimee-imposed quotas but coexists.

## §1 Budget model (kb-side)

Per-team budget rows: `{team_id, project_id?, period (day/month), limit_usd,
soft_limit_usd?}`. "Remaining" = `limit_usd − spend_in_period` where spend comes
from the P3 rollup (or a direct `token_audit` sum if P3 hasn't landed). Keep the
running counter fast: a per-period `spend_counter` updated on each egress write,
not a full table scan per request. (LiteLLM uses Redis for this; at aimee's
internal scale a kb-local counter table is sufficient — do not add Redis and its
ops burden unless a measurement demands it.)

## §2 Rate limits (kb-side, keyed)

Replace the single global window with a keyed fixed-window limiter at kb egress:
per-`team` (and optionally per-`cert:CN`/per-user) RPM/TPM. Same structure as the
existing limiter, keyed by the resolved identity instead of the whole listener.

## §3 Enforcement point + typed errors

At `/v1/llm/egress` (P2), before attaching the org key:
1. Rate check (keyed) → over-limit → refuse.
2. Budget check → over hard limit → refuse; over soft limit → allow + flag.
Refusals return a **typed, ≥1000 aimee error code** (per the aimee error-code
convention) with the offending dimension named (`team budget exceeded`,
`team rate limit`), so clients can distinguish a quota refusal from a vendor error
or an entitlement denial (P2). Soft-limit crossings emit an operator signal
(webhook/console flag) rather than blocking.

## §4 Admin surface

- `aimee budget {set,show}` → `/v1/budget/*` on kb (org-admin gated, OpenAPI +
  coverage). Team leads read their own; org admins set.
- Console: budget panel alongside the P3 spend view.

## Acceptance criteria

- A team over its hard budget is refused at kb with the typed code; a personal
  `direct` call is unaffected (no team, no cap).
- A team over its rate window is refused; other teams are unaffected (keyed, not
  global).
- Soft-limit crossing allows the call and raises exactly one operator signal.
- Budget "remaining" reconciles with the P3/`token_audit` spend for the period.
- The refusal path never leaks the org key and never reaches the vendor.

## Testing

Unit: budget arithmetic (period boundaries, soft vs hard), keyed rate window,
typed-error selection, counter vs. authoritative-sum reconciliation. Integration:
two teams, one over budget / one under, concurrent load to prove the limiter is
keyed and the counter stays consistent across kb workers.

## Non-goals

No Redis unless measured necessary. No cross-provider cost optimization/routing.
No per-request billing — this is enforcement over the spend P3 already attributes.
