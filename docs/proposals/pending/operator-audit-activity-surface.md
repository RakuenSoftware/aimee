# Proposal: First-class operator-audit activity surface

- **State:** proposed (pending — not started)

## Thesis

Every shareable row in DB2 already carries the provenance needed to answer "who
did what, in which scope, when" — `operator_id`, `content_hash`, timestamps — and
a WORM audit ledger records privileged actions. But there is **no legible way to
read it out.** The `aimee audit` verb only *verifies/checkpoints* the WORM store's
integrity; it cannot render per-operator or per-scope activity. So the data is
audit-*able* but not audit-*ed*: answering "what has operator X written to project
Y this week?" means hand-writing SQL against DB2. The three-db-split Known
Weaknesses section flagged this; it is still unbuilt.

## Goal

An operator can run one command (and one `/v1` call) to get a legible,
scoped activity report — per operator, per scope (project / workspace / global),
over a time window — sourced from the provenance columns and the WORM ledger that
already exist, with no new write path.

## §0 What already exists

- **Provenance columns.** `operator_id` is on shareable rows across
  `src/db2/` (`fidelity.c`, `corpus_structural.c`, `artifacts.c`, memory rows,
  …), alongside `content_hash` + timestamps.
- **WORM ledger.** `src/audit_ledger.c` + `src/db2/kb_audit_worm.c` record
  privileged/append-only actions with a verify chain.
- **The CLI verb exists but is integrity-only.** `cmd_audit`
  (`{"audit", "Verify/checkpoint the WORM audit store …"}`) has `verify` +
  `checkpoint` subcommands — nothing that reports activity.

Everything needed to *read* is present; only the read surface is missing.

## §1 DB2 read API: scoped activity query

Add a read-only DB2 accessor (in the owning `src/db2/` module, behind the KB
service — server/CLI must not touch DB2 directly per the storage boundary) that
aggregates activity by `(operator_id, scope, action, day)` over a time window,
unioning the provenance columns and the WORM ledger. Read-only; no new table.

## §2 `/v1/audit/activity` endpoint

Expose it as `GET /v1/audit/activity?operator=&scope=&since=&until=` on aimee-kb,
returning the scoped rollup. Add it to the OpenAPI surface + `v1-method-coverage`
so it is a first-class, conformance-tested route rather than a side door. Gate it
behind the same admin capability the other privileged `/v1/audit/*` operations
use.

## §3 `aimee audit activity` subcommand

Add an `activity` subcommand to the existing `audit` verb that calls the route and
renders a legible table (operator × scope × action counts, most-recent activity,
optional `--json`). Keep `verify`/`checkpoint` unchanged. This is the surface the
Known Weaknesses note asked for.

## §4 (optional) Anomaly summary

A `--anomalies` flag that surfaces the obvious ones the rollup makes cheap:
writes by an operator with no prior activity in a scope, a spike vs. that
operator's trailing median, or a WORM-chain gap. Not intrusion detection — just
making the legible data legible.

## Non-goals

No new capture (the data is already recorded), no tamper-evidence changes (the
WORM chain stays as-is), and no multi-tenant policy engine — this is a *read*
surface over provenance we already keep.
