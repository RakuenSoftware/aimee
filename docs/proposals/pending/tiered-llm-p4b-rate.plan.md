# P4b implementation plan — keyed fixed-window rate limiter (P4 §2)

Slice P4b of P4. Branch off `testing` (P1, P3a, P2a, P3b, P4a, P10 s1/s2/s3b merged). The
keyed rate limiter deferred from P4a. Replaces the single global bucket (`server_http_rate
_check`) with a **shared-Postgres keyed fixed-window limiter** at kb egress: per-`(dimension,
window)` RPM, so many stateless kb instances share ONE window (a per-instance RAM limiter
would grant N× the limit — invariant #9). Testable standalone (the enforcement call rides
with the deferred P2b egress). RPM only; TPM-as-reservation (reuses P4a's reserve/settle on
a token counter) is a documented follow-up.

## Verified substrate

- P4a merged: `is_team_lead`, `kb_principal_is_admin`, `kb_audit_worm_append` (atomic WORM),
  ENABLE-not-FORCE RLS + definer pattern, the surrogate-id-PK + expression-UNIQUE-INDEX
  convention, and the parallel-concurrency harness pattern (`p4_budget_concurrency.sh`).

## Design decisions

1. **Authoritative policy, never caller-supplied.** `org_rate_policy(dim, scope_key,
   window_seconds, max_count)` is **admin-set**. `org_rate_check` looks up the policy — the
   caller supplies only the resolved identity (dim_key), NOT the window or limit (closes the
   P4a-review "caller spoofs window/limit" finding). A `scope_key='*'` row is the per-dim
   default; a specific `scope_key` (e.g. team id) overrides.
2. **DB-derived window id** — `window_id = floor(extract(epoch from now()) /
   window_seconds)::text` (canonical Postgres primary clock; a request never picks its
   window). Fixed-window (not sliding) — matches the proposal.
3. **Atomic bump-and-check, shared counter.** `org_rate_window(dim_key, window_id, count)`
   (surrogate id PK + UNIQUE INDEX on `(dim_key, window_id)`). The bump is one statement:
   `INSERT INTO org_rate_window(dim_key, window_id, count) VALUES (:k, :w, 1) ON CONFLICT
   (dim_key, window_id) DO UPDATE SET count = org_rate_window.count + 1, updated_at = …
   WHERE org_rate_window.count < :max RETURNING count` — a returned row = admitted; **0
   rows = over-limit** (refuse). Shared by all N instances, so the window is the intended
   limit, never N×.
4. **Multi-dimension all-or-nothing (most-restrictive binds).** `org_rate_check` takes the
   set of applicable `(dim, scope_key)` for the request (team [+ project/cert/model/
   cred_slot when policies exist), bumps EACH in one txn in **deterministic dim_key order**;
   if ANY bump returns 0 rows (over its policy), **RAISE** → the txn rolls back → NO
   dimension is consumed (a refused request costs no window budget), and returns the typed
   refusal naming the binding dimension.
5. **Typed error** — `team rate limit` (≥1000 aimee code), distinct from budget/entitlement/
   vendor errors (mirrors P4a's typed refusals).
6. **RLS = ENABLE-not-FORCE (P3a/P4a-consistent).** `org_rate_policy` read = admin OR
   `is_team_lead` of the scope's team (a team-lead sees their team's limits); writes
   admin-gated + WORM-audited via definer. `org_rate_window` is definer-written; runtime has
   no direct DML. Window rows have no direct tenant read (operational counters).

## Scope (P4b)

1. **DB2 schema** (`db2/schema.sql` + sqlite mirror + grants):
   - `org_rate_policy(id BIGINT PK, dim TEXT CHECK(dim IN('team','project','cert','model',
     'cred_slot')), scope_key TEXT NOT NULL, window_seconds INT CHECK(>0), max_count BIGINT
     CHECK(>=0), created_at, updated_at)` + UNIQUE INDEX `(dim, scope_key)`.
   - `org_rate_window(id BIGINT PK, dim_key TEXT, window_id TEXT, count BIGINT DEFAULT 0,
     updated_at)` + UNIQUE INDEX `(dim_key, window_id)`.
   - ENABLE RLS; policy read (admin OR is_team_lead for `dim='team'` scope); REVOKE runtime
     direct DML.
2. **Definer functions** (SECURITY DEFINER, SET search_path=public, REVOKE FROM PUBLIC +
   GRANT EXECUTE runtime):
   - `org_rate_window_id(p_window_seconds INT) → TEXT` (DB-derived floor(epoch/window)).
   - `org_rate_check(p_dims TEXT[])` — for each applicable dim_key in `p_dims`, look up its
     policy (skip a dim with no policy), derive its window_id, atomic bump-and-check; on any
     over-limit RAISE (rollback all) returning `refused:<dim> rate limit`; else `admitted`.
     (Simpler single-dim variant `org_rate_check_one(dim, scope_key)` for tests.)
   - Admin `org_rate_policy_set(dim, scope_key, window_seconds, max_count)` (kb_principal_is_admin,
     WORM-audited, upsert) + `org_rate_policy_show(dim?, scope_key?)` (actor-bound read).
3. **Typed error code** `team rate limit` wired into the kb error convention.
4. **`/v1/rate/{policy,show}`** on kb (`kb/http/kb_http_rate.c`) OR fold into
   `/v1/budget/*` — admin sets policy, team-lead reads own; OpenAPI + coverage. `aimee-kb
   rate {set,show}` operator CLI.
5. **Tests**: unit (window-id math, typed-error selection) + real-PG gate
   `scripts/p4b_rate_rls_test.sql`: (a) a single window admits exactly `max_count` bumps
   then refuses; (b) **keyed** — team A over-limit does NOT affect team B (distinct dim_key);
   (c) window rollover — a new window_id resets the count; (d) multi-dim all-or-nothing (a
   refused dim consumes no window); (e) policy read RLS (team-lead own only); (f) admin-only
   policy set. PLUS `scripts/p4b_rate_concurrency.sh` (like the P4a harness): N PARALLEL
   `psql` bumps against one window whose max=K → exactly K admitted, N−K refused, count==K
   (the shared window is NOT N× the limit across connections). Wired into `run-p1-rls-gate.sh`.

## Explicitly deferred

The rate-check WIRING at `/v1/llm/egress` (P2b); TPM-as-reservation (reuses P4a
reserve/settle on a token counter — a follow-up); per-cred_slot/per-model policy resolution
detail (needs P2/P7 cred_slot_ref — the schema supports the dims now, resolution lands with
P2b); counter sharding for a hot window (only if measured); the server-proxy `aimee rate`
over mTLS (P5).

## Gate

- `make -j server` links clean; `make lint` + `make schema-sync-check` green;
  `/v1/rate/*` (or the budget-folded route) in the OpenAPI/v1 descriptor.
- Unit + the real-PG p4b gate + the parallel over-limit harness pass on CT103 (the
  shared-window-not-N× invariant is the headline). Existing gates unchanged (**re-push the
  UPDATED schema_grants.sql to CT103**).

## Non-goals (P4b)

No egress wiring (P2b), no TPM reservation (follow-up), no per-instance RAM limiter (the
thing being replaced), no counter sharding, no server-proxy CLI. Pure keyed fixed-window
RPM limiter over shared Postgres + admin, with the shared-window concurrency invariant
proven on real PG.
