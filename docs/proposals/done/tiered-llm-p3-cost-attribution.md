# Proposal: P3 — Per-team/project cost attribution at the kb egress point

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** P1 (teams), P2 (kb egress seam). **Blocks:** nothing (P4 reuses
  the same rollup but does not require P3).

## Thesis

"Enough usage visibility to see who's spending what" is one of the three things the
origin ask cares about. aimee's cost accounting is already strong; the only
missing dimension is **team/project**. This is the cheapest, highest-visibility
packet in the series: two columns, two aggregations, and the existing metering
point and dashboards.

## Goal

Attribute every org LLM call to `(team, project, user, model)` and extend the
existing insights/dashboard surfaces to answer "what did team X spend this
month," broken down by project and by model and exportable.

## §0 What already exists

- **`token_audit` DB1 table** (`src/db1/schema.sql:40`, `src/db1/token_audit.c`):
  per-request prompt/completion/cache tokens, `estimated_cost_usd`, `model`,
  `served_model`, `principal`, `session_id`, `delegation_id`, plus aggregations
  `_by_model`, `_by_source`, `_by_role`, `_by_tool`, `_spend_breakdown`.
- **Cost calc** — `token_estimate_cost_ex` with 3-tier pricing (static →
  registry → authoritative DB1 `model_pricing`); `token_billable_model`
  resolves to the real billing model, never the agent name.
- **Read surfaces** — `GET /v1/insights/overview` (`src/server_insights.c`),
  `/v1/dashboard/*`, MCP `dashboard_metrics`, React `CostPanel` ("top
  sessions").

The machinery is in place. It lacks a team dimension, and the org-call rows
are written on the server today rather than at the kb egress point.

## §1 Add the team/project dimension

Create a **DB2 `org_token_audit`** table (per-request tokens→USD like the
server's DB1 `token_audit`, plus `team_id`, `project_id`, and the pricing
`version`). At the kb egress point (P2 `/v1/llm/egress`) the caller's team and
project are already resolved (P1), so kb writes each org row there with those
fields populated. The server's DB1 `token_audit` is **left unchanged** and keeps
recording personal `egress: direct` calls (no team). Both are additive, reversible
migrations (master-plan constraint) — org rows never go into the server's DB1 table.

Each org row also carries the **authoritative originating server `cert:CN`** and,
separately, the **user identity only when a kb-verified actor token was present** (else
null — invariant #7's composite tagging, immutable fields). Every billable call
produces **exactly one** row via the durable request lifecycle of P2/P4/P7. The row's
idempotency/uniqueness key is the **composite `(origin_cert_cn, request_id)`** (a bare
request_id could collide/replay across mutually-untrusted origins), and the first
accepted request binds the immutable team/project/pricing-version. The lifecycle has
explicit states — `started → settled(success|denied|failed)` plus an **`indeterminate`**
terminal for "vendor billed but we crashed before recording actual usage": that case
settles at the **reserved max** (P4) and is reconciled downward if a late signal
arrives, so a crash/retry/streaming-disconnect can neither drop nor double-count
attribution. `org_token_audit` and its rollups are **tenant tables under invariant #10**:
`FORCE ROW LEVEL SECURITY` + a non-owner/non-`BYPASSRLS` runtime role, with the
**transaction-local, server-derived tenant context established only after primary
authorization** (P1 §1), enforce cross-tenant isolation at the DB layer (not just the
API); every access-gating read hits the primary (invariant #9).

**Where the org rows live:** org egress happens on kb, so org cost rows are
written to a dedicated **DB2** org-token-audit table (per the implementation-plan
storage boundary — the kb tier is Postgres/DB2, and the server must not touch DB2
directly). This is a **distinct store** from the server's DB1 `token_audit`, which
is left **unchanged** and continues to record personal `direct` calls. The two are
never merged in place; a combined "user's total spend" view is reconciled at the
read/rollup layer (§3), not by cross-writing tiers. The DB2 schema (columns,
migration, pricing dependency, and query API) is specified explicitly in P3a rather
than left as "DB2 or kb-local DB1." Org spend data belongs to the org, consistent
with the tiering invariant.

**Pricing is Postgres-authoritative on kb.** Cost is computed at the kb egress
point, so the price table the org tier meters against must live in Postgres (DB2),
not the server's DB1: P3a stands up a DB2 `org_model_pricing` table (model,
provider, unit prices, **version**, effective-at), seeded by promoting the existing
static/registry/`model_pricing` prices, and every stateless kb instance reads it
**consistently and versioned** — a price change is a new row/version, never an
in-place mutate, so concurrent instances always agree on the price for a given
`(model, version)`. A per-model `current_version` pointer is advanced atomically on a
price change; a call pins the version it read at reservation and uses that same
version through settlement. P2 metering, P4 maximum-cost reservations, and P6 Bedrock
pricing all resolve against this one authoritative table — never a per-instance or
server-local price.

## §2 Two aggregations + rollup

Add **kb-native** `by_team` / `by_project` aggregations over the DB2 org-audit
table (mirroring the *shape* of the server's existing DB1 `_by_model`, but as
kb/DB2 query APIs — not the DB1 function symbols, which stay server-side for
personal spend), plus a `(team, project, model, day)` rollup covering the
reporting window. The rollup is maintained **incrementally within the same DB2
transaction** as the audit-row write, so reads are cheap and always consistent with
the ledger; ad-hoc/backfill queries may compute directly over `org_token_audit`. The rollup upsert contends only **per `(team, project, model, day)` key**, not a global row; a hot key uses the same fixed-shard-sum option as the P4 counter, and only if a measurement shows single-row contention.

## §3 Reporting surface

- `GET /v1/insights/spend?team=&project=&since=&until=` on aimee-kb (org
  spend), added to OpenAPI and coverage. **Team-lead** is a P1 capability bound to a
  specific team (stored, RLS-constrained, resolved from composite identity on the
  primary), not an ad-hoc label; RLS enforces that a team-lead reads only their team's
  rows at the DB layer. **Access matrix (explicit):** an **org-admin** may query any
  authorized org scope; a **team-lead** may query **only** teams the primary currently
  grants them — the predicate is `org_admin OR team_lead_of(requested_team)`, not
  org-admin-only. A team
  lead sees their own team; an org admin sees all (reuse P1 resolution).
- Extend `CostPanel` with a team/project breakdown on the org tier view;
  `--json` export for finance.
- CLI: `aimee spend --team X [--project Y] [--since …]` reports **org** spend by calling kb's `/v1/insights/spend` over mTLS — the CLI runs on the *server*, which **never queries DB2 directly** (storage boundary); kb is the only tier that touches org rows. A user's personal `direct` spend stays in the server's local `token_audit` and is shown by the existing local cost surfaces; the two are **not silently merged**. A combined "everything I spent" view, if offered, dedupes by tier (org rows are DB2, personal rows are the server's DB1 — no overlap) and labels each source.

## Acceptance criteria

- An org call writes a **DB2 `org_token_audit`** row carrying resolved
  `team_id`/`project_id`; a personal `direct` call writes to the **server's DB1
  `token_audit`** unchanged (that table has no team/project columns — the org
  dimension exists only in DB2).
- `/v1/insights/spend?team=X` returns that team's realized spend, per project
  and per model, matching the sum of its rows.
- A team lead can read their team's spend; cannot read another team's (authz).
- `--json` export round-trips; totals reconcile with `_by_model`.
- Every kb instance prices a given `(model, version)` identically from the DB2
  `org_model_pricing` table; a price update is observed as a new version, never a
  mid-flight change to an in-flight call's price.

## Testing

Unit: attribution write (team set / NULL), the two aggregations, authz
scoping.
Integration: drive N org calls across 2 teams through kb egress; assert the
per-team rollup and the cross-team authz denial.

## Non-goals

No caps (P4). **One pricing rule, no ambiguity:** every kb org call and P4 reservation
atomically pins an existing DB2 `org_model_pricing` version row from the primary, and
settlement references that **exact immutable row** (foreign key). The server's existing
3-tier resolver stays **only for personal `direct` (DB1) pricing** — it is not a second
source for org calls. Not a
billing / invoicing system — this is a read surface over spend aimee
already computes.
