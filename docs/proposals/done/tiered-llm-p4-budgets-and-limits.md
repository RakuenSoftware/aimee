# Proposal: P4 — Budgets + rate limits (turn tracked spend into enforced caps)

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** P1 (teams), P2 (kb egress seam), and **P3a** — the hard-budget algorithm meters against P3's Postgres `org_model_pricing` and settles realized spend through the DB2 `org_token_audit` machinery, so P3a's schema/pricing is a prerequisite, not optional.

## Thesis

aimee tracks spend but never caps it, and its only rate limiter is a single global 60-second bucket (`server_http_rate_check`, `src/server/server_http.c:282`) — not per-team, not per-key. "Something that won't need a full-time engineer babysitting it" means a POC team cannot accidentally burn the month's budget on a runaway loop. This packet adds enforcement at the one seam P2 created (kb egress), where team identity and cost are both already in hand.

## Goal

Per-team (and optional per-project) budget caps and rate limits enforced at kb egress: a call that would exceed a team's remaining budget or rate window is refused with a clear, typed error before it reaches the vendor.

## §0 What already exists

- Spend is computed per call — `estimated_cost_usd` via `token_estimate_cost_ex` (P3 §0); at kb egress the team is resolved (P1) and the cost is known.
- Cost caps exist for ensembles/roundtable — `config.h:1805,1826`, `roundtable_preset.h:47` — a precedent for pre-flight cost gating, but scoped to internal orchestration, not per-team ingress.
- A global rate limiter — `server_http_rate_check` (one bucket) — is the thing to replace with a keyed limiter, not extend.
- Provider-side cooldown/rotation — `delegate_credentials.c` handles upstream 429s; unrelated to aimee-imposed quotas but coexists.

## §1 Budget model (kb-side)

Per-team budget rows: `{team_id, project_id?, period (day/month), limit_usd, soft_limit_usd?}`. Remaining = `limit_usd − spend_in_period`, where `spend_in_period` comes from the P3 rollup, or from a direct `token_audit` sum if P3 has not landed.

- **Team and project caps both apply (cumulative).** When a project cap is set, a call must fit under **both** its team period row and its project period row: T1 reserves against both rows in **one primary transaction** under a deterministic lock order (team then project), and rolls back **all** reservations if any constraint fails; settlement releases both.
- **Budget edits are primary-only transactions** that conditionally update the pinned period row. A hard-limit **reduction below realized spend + active reservations is rejected** as retroactive; a lowered limit applies to **new admissions only** — in-flight reservations are honored, and no new call is admitted until realized + reserved is under the new limit. Edits name the period they affect (current vs. next).
- **TPM (tokens-per-minute) uses the same reservation discipline** as spend: a primary-only reservation ledger keyed by the applicable identities + a **DB-derived window id** pinned at admission, reserving the maximum billable tokens atomically, settling to actual (full-charge on unknown/expiry), with the same lease/retry semantics as budget reservations — not a hand-wavy "analogous".

Keep the running counter fast: a per-period `spend_counter` **row in shared Postgres** (invariant #9 — never kb-instance-local), updated on each egress write by an atomic increment on the **primary**, not a full table scan per request. Every stateless aimee-kb instance shares that one row, so the balance is correct no matter how many instances race. (LiteLLM uses Redis for this; at aimee's scale the shared Postgres counter is sufficient — do not add Redis and its ops burden unless a measurement demands it.)

**Hard caps use atomic reservations, not a post-hoc counter.** Completion-token cost is unknown before dispatch, and a check-then-write counter lets concurrent kb workers all pass against the same `remaining`. So a hard budget check **reserves a conservative maximum** before vendor dispatch — derived from the known prompt tokens and the request's `max_tokens` bound (a **finite** ceiling, since `max_tokens` is always set) priced at a **pinned `org_model_pricing` version recorded on the reservation** — applied to `spend_counter` in a single atomic transaction on the **Postgres primary** (`UPDATE … SET reserved = reserved + :max WHERE limit_usd − spend − reserved ≥ :max`) — shared by all N stateless kb instances, never a per-instance counter and never decided off a lagging read replica (invariant #9); if the row does not update, the balance is exhausted and the call is refused. After the vendor call completes, fails, or is cancelled mid-stream, the reservation is **reconciled** to realized cost, releasing the unused remainder. The maintained invariant is **`Σreserved + realized_spend_in_period ≤ limit_usd`** for every team, enforced by an atomic `available = limit − spend − Σreserved` check inside T1 on the primary — so no combination of concurrent admissions from ≥2 stateless instances can over-commit the balance. T1 also **pins the budget-period id** (alongside team, project, pricing version, and reserved max); settlement reconciles against that *same* period regardless of when the call completes, so a request straddling a day/month boundary charges and releases the same period — never one period's reservation against another period's counter. Period rollover and mid-flight budget edits are defined against the pinned period. kb **requires a bounded `max_tokens`** for a hard-capped call: if the caller omits it, kb applies the model's configured default ceiling, so the reserved maximum is always finite — an unbounded request is refused, never reserved at infinity. The reserved maximum is a **per-provider/model maximum-cost function over *every* billable dimension** — prompt, max completion, cache read/write, media/tool, per-request, and any provider-specific charge — not just prompt+completion; a model whose charge cannot be conservatively bounded is **rejected for hard-capped use** rather than under-reserved. A TPM limit reserves the maximum token count analogously and reconciles to actual. Soft limits may stay approximate (counter-based); only hard caps require the reservation. Reservations carry an idempotent request id and are held by a **lease the serving instance renews (heartbeat) while the call is active**, not a bare timeout: only a *stopped* heartbeat (a crashed/hung worker) expires the lease, so a legitimately long-running stream keeps renewing and is never released while it is still spending. On completion the lease reconciles to realized cost, **priced against the same pinned version the reservation used** (so reservation and settlement never disagree on price). When a lease expires **without** reconciliation (worker crashed, realized cost unknowable), the reservation **settles at its full reserved max** — the conservative charge that preserves the hard cap — and is only ever adjusted *downward* if a late reconcile arrives. A live stream keeps renewing its lease and is never prematurely freed; a dead worker's reservation is never silently released.

## §2 Rate limits (kb-side, keyed)

Replace the single global window with a keyed fixed-window limiter at kb egress: per-`team` (and optionally per-`project`, per-`cert:CN`/per-user, per-model, **and per org credential-slot** — the stable P2/P7 `cred_slot_ref`, so a shared upstream key can be rate-limited without exposing key material) RPM/TPM, keyed by the resolved identity instead of the whole listener; the policy states how these dimensions compose (the most restrictive binding wins). **The window state lives in shared Postgres** (invariant #9), not per-instance memory: with many stateless kb instances, a per-instance in-RAM limiter would let each instance grant the full window, so the effective limit would be N× the intended one. Each request's window bump is an atomic write on the primary; a per-instance RAM limiter is explicitly wrong here. Counter/window rows are keyed **per (team, period)**, so contention is per-team rather than one global hot row; a very hot team can shard its counter into a fixed set of sub-rows summed on read — but only if a measurement shows single-row contention (do not pre-optimize). (The old single-process `server_http_rate_check` bucket is the pattern to *replace* for the org egress limiter, not to key per-instance.) This keyed limiter is **added at kb egress**; the aimee-server's own global ingress limiter stays in place to protect the server and personal `direct` traffic — P4 does not remove the server-side limiter, it adds a shared org one on kb.

## §3 Enforcement point + typed errors

At `/v1/llm/egress` (P2), before attaching the org key:

1. Rate check (keyed) → over-limit → refuse.
2. Budget check → **atomically reserve** the request's conservative max cost (§1); if the reservation cannot be granted against the remaining hard limit → refuse; over soft limit → allow + flag. Reconcile the reservation to realized cost when the call finishes, fails, or is cancelled mid-stream.

Refusals return a typed, ≥1000 aimee error code (per the aimee error-code convention) with the offending dimension named (`team budget exceeded`, `team rate limit`), so clients can distinguish a quota refusal from a vendor error or an entitlement denial (P2).

**Reservation vs. audit are separate ledgers; dispatch is a durable state machine.** Immutable factual usage-audit rows (P3 `org_token_audit`) are kept **separate** from the conservative budget-reservation ledger: on reservation expiry with unknown outcome, append a *full-reservation settlement* to the budget ledger; on a late reconcile, append a **compensating adjustment** — never mutate a prior row. The request is a durable state machine (`admitted → dispatch-authorized → dispatched → outcome`, P7 §6): a request whose dispatch outcome is **uncertain is never auto-redispatched** (it uses the vendor idempotency key where the provider supports one, else settles at reserved max and surfaces as failed), so a crash after vendor acceptance but before durable settlement cannot double-charge or double-call.

**Soft-limit signals are edge-detected and deduplicated.** The threshold crossing is recorded in T1 under a unique key (`budget_period + threshold`) so **only the crossing transaction emits** exactly one operator signal; the notification is enqueued via a transactional outbox with idempotent delivery — concurrent requests cannot each emit. Soft-limit crossings flag; they do not block.

**Two idempotent transactions bracket the vendor call** (not one — the vendor call is external and must never sit inside a DB transaction). **T1 (admission), committed *before* dispatch:** pin the price version, atomically place the reservation, and write the P7 WORM admission + dispatch record. **T2 (settlement), after the response (or a crash-timeout):** insert **exactly one** `org_token_audit` row, increment the rollup + realized-spend counter, and release the reserved remainder. Both are **request-id-keyed and idempotent** — the idempotency key is the composite `(origin_cert_cn, request_id)` bound to the immutable request contents (not a bare id), and the **vendor idempotency key is that same request id** (the two idempotency spaces are one). A crash or retry between them can neither double-charge, double-audit, nor orphan a reservation. The **dispatch state persists before the external call** (`admitted → dispatch-authorized` in T1, `dispatched → outcome` after), so an uncertain dispatch is never auto-redispatched; the budget ledger's conservative full-charge on an unknown outcome and T2's exactly-one factual audit row are **separate ledgers** (the audit row records realized usage or is marked indeterminate; the budget ledger holds the conservative reservation), so they never contradict.

**Canonical arithmetic and clock.** USD is stored as an exact integer minor-unit / `NUMERIC` (never float), with defined rounding and overflow-checked accumulation; the **budget period boundary uses the Postgres server clock (`now()` on the primary)** as the single canonical clock — never an instance host clock — with an explicit timezone, so N stateless instances agree on which period a call falls in. Rate/budget rows are read on the **primary** (never a lagging replica) and bumped with an atomic `INSERT … ON CONFLICT … DO UPDATE` keyed by `(dimension, period)`. Multi-dimensional rate admission is **all-or-nothing** in one primary transaction across every applicable dimension (team, project, cert:CN, model, cred-slot) — a partial grant is never left behind. **Budget-row deletion** is defined: it refuses *new* admissions immediately but does **not** cancel in-flight reservations (they settle normally); a deleted-then-recreated row starts a fresh period. `budget set/show` queries run under **FORCE RLS** team-scoped predicates (invariant #10), so cross-tenant budget reads are denied at the DB layer.

## §4 Admin surface

- `aimee budget {set,show}` → `/v1/budget/*` on kb (org-admin gated, OpenAPI + coverage). Team leads read their own; org admins set.
- Console: budget panel alongside the P3 spend view.

## Acceptance criteria

- A team over its hard budget is refused at kb with the typed code; a personal `direct` call is unaffected (no team, no cap).
- A team over its rate window is refused; other teams are unaffected (keyed, not global).
- Soft-limit crossing allows the call and raises exactly one operator signal.
- Budget "remaining" reconciles with the P3/`token_audit` spend for the period.
- The refusal path never leaks the org key and never reaches the vendor.
- Under concurrent load from many kb instances against one team's balance, total realized spend **never exceeds the hard limit** — reservations are deducted from the shared Postgres counter *before* dispatch, so the cap is hard, not best-effort (a conservative reservation may refuse a call the team could technically still afford, but never allows an overspend). A crashed worker's reservation settles at its reserved max (≥ realized cost, so still no overspend) and is never orphaned.
- The budget cap and rate window hold **across all kb instances**, not multiplied by instance count: two (or twenty) instances serving the same team share one Postgres-backed counter and window; a test runs the same team against ≥2 instances concurrently and asserts the aggregate cap/limit is not exceeded.
- Under arbitrary concurrent admission from ≥2 instances against one team, the invariant `Σreserved + realized_spend ≤ limit` holds at **all** times (tested); a request whose admission and settlement straddle a period boundary is charged and released against the single period pinned at T1.

## Testing

Unit: budget arithmetic (period boundaries, soft vs hard), keyed rate window, typed-error selection, counter vs. authoritative-sum reconciliation. Integration: two teams, one over budget / one under, concurrent load to prove the limiter is keyed and the counter stays consistent across kb workers.

## Non-goals

No Redis unless measured necessary. No cross-provider cost optimization/routing. No per-request billing — this is enforcement over the spend P3 already attributes.
