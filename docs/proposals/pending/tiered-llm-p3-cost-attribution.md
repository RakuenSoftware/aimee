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

Add `team_id` and `project_id` columns to `token_audit`. Both are nullable —
personal `egress: direct` calls have no team. At the kb egress point
(P2 `/v1/llm/egress`) the caller's team and project are already resolved
(P1), so kb writes the audit row with those fields populated. The migration
is additive, backfilled to NULL, and follows the reversible-migration
discipline (master-plan constraint).

**Where the org rows live:** org egress happens on kb, so org cost rows are
written on the kb tier (DB2-side audit, or a kb-local DB1). Org spend data
belongs to the org, consistent with the tiering invariant. Personal `direct`
calls continue writing to the server's local `token_audit`.

## §2 Two aggregations + rollup

Add `db1_token_audit_by_team` and `_by_project` mirroring the shape of the
existing `_by_model`, plus a `(team, project, model, day)` rollup covering
the reporting window.

## §3 Reporting surface

- `GET /v1/insights/spend?team=&project=&since=&until=` on aimee-kb (org
  spend), added to OpenAPI and coverage. Gated behind org-admin: a team
  lead sees their own team; an org admin sees all (reuse P1 resolution).
- Extend `CostPanel` with a team/project breakdown on the org tier view;
  `--json` export for finance.
- CLI: `aimee spend --team X [--project Y] [--since …]`.

## Acceptance criteria

- An org call writes an audit row carrying resolved `team_id`/`project_id`; a
  personal call writes one with both NULL.
- `/v1/insights/spend?team=X` returns that team's realized spend, per project
  and per model, matching the sum of its rows.
- A team lead can read their team's spend; cannot read another team's (authz).
- `--json` export round-trips; totals reconcile with `_by_model`.

## Testing

Unit: attribution write (team set / NULL), the two aggregations, authz
scoping.
Integration: drive N org calls across 2 teams through kb egress; assert the
per-team rollup and the cross-team authz denial.

## Non-goals

No caps (P4). No new pricing sources (the 3-tier resolver stays). Not a
billing / invoicing system — this is a read surface over spend aimee
already computes.
