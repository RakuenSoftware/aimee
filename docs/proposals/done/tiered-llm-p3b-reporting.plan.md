# P3b implementation plan: org spend reporting surface (P3 §2-3)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice P3b of P3 (cost attribution). Branch off `testing` (P1, P3a, P10 s1/s2/s3b, P2a
merged). P3a shipped the schema (`org_token_audit`, `org_spend_rollup`, versioned
`org_model_pricing`) + the team-lead/admin RLS read policies (`is_team_lead`,
`p_rollup_read`, `p_audit_read`). **P3b is the READ/reporting layer**: the kb-native
aggregations + `GET /v1/insights/spend` + a CLI, team-lead scoped. Testable against
SEEDED rollup rows. No P2b live egress needed for the read side (the write/settle path
that populates the rollup rides with P2b; P3b reports over whatever rows exist).

## Verified substrate (from P3a)

- `org_spend_rollup(team_id, project_id, billable_model, day, prompt_tokens,
  completion_tokens, cache_read_tokens, cache_write_tokens, cost_usd NUMERIC(20,10),
  row_count, updated_at)` — ENABLE RLS; `p_rollup_read USING (kb_principal_is_admin()
  OR is_team_lead(team_id))`. `org_token_audit` (per-request ledger) similarly gated.
- `is_team_lead(p_team)` reads `current_setting('aimee.principal')` (the actor).
- The P2a pattern to mirror: actor-bound SECURITY DEFINER read functions (no principal
  arg. Read `aimee.principal`), runtime SELECT funnelled through the definer, kb HTTP
  route via `kb_reqctx` + `db2_tenant_scope`, real-PG RLS gate.

## Design decisions

1. **Actor-bound aggregation definer functions** (SECURITY DEFINER, no principal arg.
The access predicate is `kb_principal_is_admin() OR is_team_lead(p_team)` evaluated on
   `current_setting('aimee.principal')`), so a caller can never read a team they don't
   lead. `org_spend_query(p_team BIGINT, p_project BIGINT NULL, p_since TEXT, p_until TEXT)`
   returns the per-`(project, billable_model)` breakdown (tokens + cost_usd + row_count)
   over `org_spend_rollup` for `day ∈ [since, until]`, RAISE (insufficient_privilege) if
   the actor is neither admin nor a lead of `p_team`. `p_team IS NULL` (org-admin only)
   returns all teams. Reads the ROLLUP (cheap); an `org_spend_query_audit(...)` variant
   over `org_token_audit` is the ad-hoc/exact path (same authz).
2. **`day` range as TEXT**: `day` is `'YYYY-MM-DD'` TEXT (P3a), so `since`/`until` are
   TEXT ISO dates and the range is a lexicographic `BETWEEN` (valid for zero-padded ISO).
   Validate the inputs are well-formed dates.
3. **RLS posture**: ENABLE-not-FORCE (P3a). The definer functions bypass to aggregate,
   but enforce the admin/lead predicate INTERNALLY (so the function IS the authz gate);
   runtime's direct `org_spend_rollup` SELECT stays RLS-filtered (defense-in-depth). No
   new tables, pure read layer.
4. **Response = per-project + per-model** (acceptance): totals reconcile (`sum over
   models == team total`). `--json` shape stable for finance export.

## Scope (P3b)

1. **DB2 read functions** (`db2/schema.sql`): `org_spend_query(p_team, p_project, p_since,
   p_until)` + `org_spend_query_by_model(...)` (or one function returning rows the C
   layer groups), actor-bound authz, aggregate over `org_spend_rollup`. `REVOKE ALL ...
   FROM PUBLIC`; `GRANT EXECUTE` to runtime (schema_grants.sql). Typed C access layer
   `db2/org_spend.{c,h}` (kb-only) over the definer via `db2_conn`/`aimee_pg_*`.
2. **`GET /v1/insights/spend?team=&project=&since=&until=`** (`kb/http/kb_http_insights.c`,
   new): actor from `kb_reqctx`, tenant scope, resolve/authorize the requested team
   (admin or lead), return spend JSON (`{team, project?, since, until, total:{tokens,
   cost_usd, calls}, by_model:[...], by_project:[...]}`). `403` if not authorized, `400`
   on a bad date/team. Wire into the kb router + OpenAPI-v1 + coverage.
3. **CLI** `aimee-kb spend --team X [--project Y] [--since] [--until] [--json]` (operator,
   in-process db2 as owner), mirrors the P1/P2a operator CLI. The **server-proxy
   `aimee spend`** (server→kb over mTLS, §3) is a **P5 follow-up** (same remote-actor
   forwarding the P1 `aimee team` remote CLI deferred), noted not built here.
4. **Tests**: unit (date validation, authz predicate, per-model reconciliation) + real-PG
   gate `scripts/p3b_spend_rls_test.sql` (p3a-style): seed `org_spend_rollup` rows for 2
   teams + a lead of each; assert a team-lead sees ONLY their team's spend (cross-team
   query raises insufficient_privilege), an org-admin sees all, per-model totals sum to
   the team total, a date-range filter works. Wired into `run-p1-rls-gate.sh`.

## Explicitly deferred

The org row WRITE on a live call + rollup population (P2b `/v1/llm/egress`); the
server-side `aimee spend` CLI over mTLS + CostPanel/React org-tier breakdown (P5/UI
follow-up); budget caps (P4). P3b is read-only over existing rollup rows.

## Gate

- `make -j server` (builds kb+server) links clean; `make lint` (kb-target-isolation,
  v1-route-order, api-conformance, module-boundary, cli-v1-routes) + `make
  schema-sync-check` green; `/v1/insights/spend` in the OpenAPI/v1 descriptor
  (docs-gen / v1-method-coverage green).
- Unit + the real-PG p3b gate pass on CT103 (team-lead isolation, admin-all, per-model
  reconciliation, date range). Existing gates unchanged (**re-push the UPDATED
  schema_grants.sql to CT103**, a stale grants file masked a REVOKE twice before).

## Non-goals (P3b)

No caps (P4), no live-call write (P2b), no server-proxy CLI, no billing/invoicing. Pure
authorized read surface over spend aimee already computes.

## v2 refinements (roundtable-converged; simpler + hardened)

- **DROP `org_spend_query_audit` from P3b**: ship ONLY `org_spend_query` over
  `org_spend_rollup` (one source, no rollup-vs-ledger drift contract; the rollup is
  maintained in the P3a settle txn so it IS the authoritative reporting source). The
  ad-hoc exact-ledger query is deferred to when it's needed, with an explicit
  `source`/`settled_through` watermark at that time.
- **`p_team IS NULL` (all-teams) branch requires `kb_principal_is_admin()` EXPLICITLY**: do not rely on `is_team_lead(NULL)` being false. The predicate is:
  `IF p_team IS NULL THEN require admin ELSE require (admin OR is_team_lead(p_team))`.
  `is_team_lead(NULL)` returns false (documented), but admin is checked explicitly for
  the NULL path.
- **Strict date validation, fail-closed on the same RAISE path as authz.** `p_since` /
  `p_until` must match `^\d{4}-\d{2}-\d{2}$`, cast validly to `::date` (catch invalid like
  `2026-13-40`), and satisfy `p_since <= p_until`; otherwise RAISE (errcode 22007/22023).
  The C route rejects a malformed team/date at the boundary (400) before the definer call.
- **`cost_usd` emitted as a deterministic NUMERIC string** (e.g. `to_char(cost_usd,
  'FM9999999990.0000000000')` / the numeric's text form), NEVER a C double, no precision
  loss for finance export. tokens/calls are integers.
- **Explicit `total` field.** Response: `{team, project?, since, until,
  total:{prompt_tokens, completion_tokens, cache_read_tokens, cache_write_tokens,
  cost_usd, calls}, by_model:[{billable_model, …same fields…}], by_project:[…]}`. The gate
  asserts `sum(by_model.cost_usd) == total.cost_usd` and per-model tokens reconcile.
- **Route team/date handling:** `?team=` OPTIONAL int64. Absent → the `p_team IS NULL`
  (admin-only) branch; present → parsed/validated int64 (reject non-integer/out-of-range,
  400). `?project=` optional int64. `?since=/?until=` required (or default to a bounded
  window), validated as above.
- **Gate proves the FUNCTION is the gate (not just table RLS):** call `org_spend_query`
  as a non-admin, non-lead actor (via `set_tenant_context`) and assert it RAISEs
  `insufficient_privilege`, since the SECURITY DEFINER bypasses RLS, its INTERNAL
  admin/lead predicate is the only authz on that path, so it must be tested directly.
  Plus: team-lead sees only own team, admin sees all, per-model reconciliation, date range.
- **P4 coupling note:** `total.cost_usd`/`calls` is the signal P4's budget enforcement
  will read; keep the response shape stable so P4 can add `cap`/`remaining` without a
  breaking change.
