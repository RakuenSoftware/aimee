# P4a implementation plan: budget/reservation/rate core (P4 §1-4, minus egress wiring)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice P4a of P4 (budgets + rate limits). Branch off `testing` (P1, P3a, P2a, P3b, P10
s1/s2/s3b merged). P4 enforces caps **at kb egress**, but the egress point is P2b
(deferred). P4a builds the **enforceable core** that P2b will call: the budget/reservation/
rate schema + the atomic `reserve`/`settle`/`rate-check` SQL functions + the admin surface,
with the shared-Postgres-counter concurrency invariant proven on real PG17. The actual
`reserve-before-dispatch` / `settle-after-response` WIRING at `/v1/llm/egress` rides with
P2b; P4a is the algorithm + storage + admin, testable standalone (like P3a).

## Design decisions (the hard-cap algorithm: P4 §1,3)

1. **Shared-Postgres atomic reservation, not a post-hoc counter** (invariant #9). A hard
   budget check RESERVES a conservative maximum cost BEFORE dispatch, in ONE primary txn:
   `UPDATE org_budget_counter SET reserved = reserved + :max WHERE limit_usd - spend -
   reserved >= :max RETURNING …`, if 0 rows updated, the balance is exhausted → refuse.
   The maintained invariant is **`Σreserved + realized_spend ≤ limit_usd`** for every
   `(team, period)`, so no combination of concurrent admissions from ≥N stateless
   instances can over-commit. `reserved_max` is a **finite** ceiling from prompt tokens +
   the request's bounded `max_tokens` priced at a **pinned `org_model_pricing` version**
   recorded on the reservation (a model whose cost can't be conservatively bounded is
   rejected for hard-capped use, not under-reserved).
2. **Team + project caps cumulative, deterministic lock order.** When a project cap is
   set, a call must fit BOTH the team and project period rows: `org_budget_reserve` locks
   **team then project** (deterministic order → no deadlock), reserves against both in one
   txn, rolls back ALL on any failure.
3. **Period pinned at T1.** The reservation pins the **budget-period id** (from the
   Postgres server clock `now()` on the primary, the single canonical clock, never an
   instance host clock) alongside team/project/pricing-version/reserved-max, so
   settlement reconciles against that SAME period even if the call straddles a
   day/month boundary.
4. **Lease + reconciliation.** A reservation carries an idempotent
   `(origin_cert_cn, request_id)` key + a **lease with a heartbeat** the serving instance
   renews while the call is active. `org_budget_settle(origin_cn, request_id, realized)`
   reconciles: reservation → settled, `reserved -= reserved_max`, `spend += realized`
   (priced against the SAME pinned version). A lease that **expires without reconcile**
   (crashed worker) settles at its **full reserved_max** (the conservative charge that
   preserves the hard cap), adjusted DOWNWARD only if a late reconcile arrives. Idempotent
   on the composite key (a retry appends nothing / no double-charge).
5. **Keyed rate limiter (P4 §2), shared Postgres.** Replace the single global bucket with
   a per-`(dimension, window)` fixed-window counter in Postgres (dimension = team [+
   optional project/cert:CN/model]), keyed by resolved identity. Window id is
   **DB-derived** (server clock). Atomic `INSERT … ON CONFLICT (dim, window) DO UPDATE SET
   count = count + 1 WHERE count < :limit RETURNING`, 0 rows → over-limit → refuse.
   Multi-dimension admission is all-or-nothing in one txn (most-restrictive binds).
6. **Canonical USD**: `NUMERIC(20,10)` exact (never float), matching P3a's `cost_usd`.
7. **RLS = ENABLE-not-FORCE (P3a-consistent).** Budget reads (team-lead sees own, admin
   all) via team-scoped SELECT policies + actor-bound definer; reserve/settle/edit via
   admin/writer definer functions (owner bypasses). (The proposal says FORCE; we use the
   proven ENABLE-not-FORCE + definer pattern that works with the non-super `aimee_kb_owner`.)

## Scope (P4a)

1. **DB2 schema** (`db2/schema.sql` + sqlite mirror + grants):
   - `org_budget(id, team_id, project_id NULL, period TEXT CHECK(day|month),
     limit_usd NUMERIC(20,10), soft_limit_usd NUMERIC NULL, created_at, updated_at,
     UNIQUE(team_id, COALESCE(project_id,0), period))`, the cap config.
   - `org_budget_counter(team_id, project_id NULL, period TEXT, period_id TEXT,
     spend_usd NUMERIC DEFAULT 0, reserved_usd NUMERIC DEFAULT 0, updated_at,
     PRIMARY KEY(team_id, COALESCE(project_id,0), period, period_id))`, the shared
     atomic counter (per (team,project,period,period_id)).
   - `org_budget_reservation(id, request_id, origin_cert_cn, team_id, project_id NULL,
     period TEXT, period_id TEXT, pricing_version BIGINT, reserved_max_usd NUMERIC,
     state TEXT CHECK(admitted|settled|expired_settled), lease_expires_at TEXT,
     realized_usd NUMERIC NULL, created_at, settled_at, UNIQUE(origin_cert_cn, request_id))`.
   - `org_rate_window(dim_key TEXT, window_id TEXT, count BIGINT DEFAULT 0, updated_at,
     PRIMARY KEY(dim_key, window_id))`.
   - ENABLE RLS; team-scoped read policies (admin OR is_team_lead) on org_budget/counter;
     REVOKE runtime direct DML (writes via definer).
2. **Definer functions** (SECURITY DEFINER, advisory-locked, WORM-audited where mutating):
   `org_budget_period_id(period)` (server-clock day/month id); `org_budget_reserve(...)`
   (atomic Σreserved+spend≤limit against team [+project], deterministic lock order, pins
   period+version, inserts reservation, returns granted/refused typed); `org_budget_settle(
   origin_cn, request_id, realized_usd)` (reconcile, idempotent); `org_budget_settle_expired(
   now)` (lease-expired → full reserved_max); `org_rate_check(dim_key, window_id, limit)`
   (atomic bump); admin `org_budget_set(team, project?, period, limit, soft?)` (reject a
   hard reduction below realized+reserved as retroactive; WORM-audited); `org_budget_show(
   team, project?)` (actor-bound read, admin OR team-lead).
3. **Typed error codes** (≥1000 aimee convention): `team budget exceeded`, `team rate
   limit`, distinct from vendor/entitlement errors. Returned by the reserve/rate refusal.
4. **`/v1/budget/{set,show}`** on kb (`kb/http/kb_http_budget.c`) + `aimee-kb budget
   {set,show}` operator CLI. org-admin sets; team-lead reads own. OpenAPI + coverage.
5. **Tests**: unit (period arithmetic, typed-error selection) + real-PG gate
   `scripts/p4_budget_rls_test.sql` proving the CRITICAL invariants: (a) **concurrent
   reservations from parallel sessions against one team's balance NEVER over-commit**.
The atomic UPDATE serializes so `Σreserved + spend ≤ limit` always holds (drive several
   reserves that together exceed the limit; assert the exact number succeed and the rest
   refuse, and the counter never exceeds limit); (b) settle reconciles (reserved down,
   spend up, idempotent); (c) an expired lease settles at reserved_max; (d) team+project
   cumulative (fits both or refused); (e) rate window keyed (team A over-limit doesn't
   affect team B); (f) budget read RLS (team-lead sees own only); (g) retroactive
   reduction rejected. Wired into `run-p1-rls-gate.sh`.

## Explicitly deferred (P2b + later)

The reserve-before-dispatch / settle-after-response WIRING at `/v1/llm/egress` (P2b); the
transactional-outbox soft-limit operator signal + console panel; the P7 WORM
admission/dispatch state machine coupling (P2b/P7); the server-proxy `aimee budget` over
mTLS (P5). P4a ships the algorithm + storage + admin, not the egress call.

## Gate

- `make -j server` links clean; `make lint` (kb-target-isolation, v1-route-order,
  api-conformance, module-boundary, cli-v1-routes) + `make schema-sync-check` green;
  `/v1/budget/*` in the OpenAPI/v1 descriptor.
- Unit + the real-PG p4 gate pass on CT103 (the concurrency over-commit invariant is the
  headline). Existing gates unchanged (**re-push the UPDATED schema_grants.sql to CT103**).

## Non-goals (P4a)

No egress wiring (P2b), no Redis, no console panel, no server-proxy CLI. Pure enforceable
budget/reservation/rate core + admin, tenant-isolated, with the shared-counter concurrency
invariant proven on real PG.

## v2 refinements (roundtable-converged; correctness-critical)

Panel found no blocking issue, but this is the most subtle slice, these reshape it:

- **NARROW P4a to the BUDGET reservation core; DEFER the keyed rate limiter to P4b.**
  The rate limiter needs its own authoritative rate-policy schema + admin (a caller must
  NOT supply window_id/limit, window is DB-derived, limit is policy-driven), which is a
  distinct sub-system. P4a = budgets only; `org_rate_window`/`org_rate_check` move to P4b.
- **CRITICAL schema fix: no expression in a UNIQUE/PRIMARY-KEY column list.** PostgreSQL
  rejects `UNIQUE(team_id, COALESCE(project_id,0), period)`. Use a surrogate `id BIGINT`
  PK on every table + a partial/expression **UNIQUE INDEX**:
  `CREATE UNIQUE INDEX ... ON org_budget(team_id, COALESCE(project_id,0), period)` (and
  likewise for the counter's `(team_id, COALESCE(project_id,0), period, period_id)`).
- **Money CHECKs + cap-preserving settle.** `limit_usd/soft_limit_usd/reserved_max/
  realized >= 0` CHECKs. On settle, the amount charged is `LEAST(realized, reserved_max)`
  per binding counter, never charge more than was reserved, so a buggy ceiling calc can't
  break `Σreserved + spend ≤ limit`. `reserved_max` MUST be a true conservative ceiling
  (realized ≤ reserved_max by construction of the per-provider max-cost function).
- **Reservation → ALLOCATIONS model (cumulative caps done right).** A reservation may bind
  MULTIPLE counters (team+project, and if both day and month caps exist, each). Add
  `org_budget_reservation_alloc(reservation_id, counter_key TEXT, period_id TEXT,
  reserved_usd NUMERIC)` — one row per counter the reserve bound. `org_budget_reserve`
  computes the applicable set (every configured cap for the (team,project) at the
  request's instant), reserves against EACH in a single txn under a **deterministic
  counter_key order** (sort the keys, covers team-then-project AND day/month AND any
  future dimension, a complete deadlock argument), rolls back ALL if any fails; settle
  releases/charges EACH allocation. (A single-cap deployment has one alloc; the common
  case stays simple.)
- **Lease state machine (DB clock, row-locked, mutually exclusive).**
  `lease_expires_at` is computed as `now() + :ttl` on the primary (never caller-supplied);
  stored as TEXT via `pg_now_text`-style but compared by casting to timestamptz, or store
  timestamptz. Use a consistent comparable type. `org_budget_heartbeat(origin_cn,
  request_id)` extends the lease `WHERE state='admitted'` (SELECT … FOR UPDATE).
  `org_budget_settle(origin_cn, request_id, realized)` and `org_budget_settle_expired()`
  both `SELECT … FOR UPDATE` the reservation and transition `admitted → settled` /
  `admitted → expired_settled`, mutually exclusive; a second settle of a terminal row is
  an idempotent no-op (a LATE reconcile after expiry adjusts spend DOWNWARD only, never
  up). `org_budget_settle_expired()` derives `now()` internally (no caller time).
- **Idempotency = read-back on the composite key** (P3a pattern). A same-`(origin_cert_cn,
  request_id)` re-reserve whose immutable triple (team, project, pricing_version,
  reserved_max) differs from the bound row is **rejected** (not silently re-reserved);
  an identical retry returns the existing reservation. Refused requests are not persisted
  (only granted reservations occupy `reserved`).
- **period_id = UTC, explicit.** `org_budget_period_id(period)` =
  `to_char(now() AT TIME ZONE 'UTC', 'YYYY-MM-DD')` for day / `'YYYY-MM'` for month. The
  single canonical clock; settlement never recomputes (uses the reservation's pinned id).
- **RLS hardening (ENABLE-not-FORCE, documented).** Every definer `SET search_path =
  public`; `REVOKE ALL … FROM PUBLIC` + `GRANT EXECUTE` to runtime only; the runtime role
  is non-owner NOBYPASSRLS and cannot `SET ROLE` the owner (P1 schema_roles), so owner
  bypass is a protected trust boundary. Budget reads (`org_budget_show`) actor-bound
  (admin OR is_team_lead).
- **Concurrency gate = genuinely parallel connections.** The real-PG gate proves
  over-commit-safety with N PARALLEL psql connections each calling `org_budget_reserve`
  against ONE team's balance (a sequential test can't exercise the lost-update race):
  seed a limit that admits exactly K of M concurrent reserves; assert exactly K succeed,
  M−K refuse, and `spend + Σreserved ≤ limit` holds. Driven by a shell harness firing
  concurrent `psql -c` in the background + a wait. Plus sequential checks: settle
  reconcile + idempotency, expired→reserved_max, team+project cumulative (both alloc rows),
  read RLS, retroactive-reduction reject.

## P2b integration contract (frozen by P4a, so P2b integrates cleanly)

- **Admit:** `org_budget_reserve(origin_cn, request_id, team, project?, pricing_version,
  reserved_max_usd) → {granted | refused:<typed reason>}`. Call in T1 BEFORE dispatch.
- **Heartbeat:** `org_budget_heartbeat(origin_cn, request_id)`. The serving instance
  renews while the stream is active.
- **Settle:** `org_budget_settle(origin_cn, request_id, realized_usd)`. After the vendor
  response; idempotent; charges `LEAST(realized, reserved_max)`.
- **Ambiguous/crash:** no call → the lease expires → `org_budget_settle_expired()` (a
  periodic sweeper) charges reserved_max; a late `org_budget_settle` adjusts down only.
- **Cancel mid-stream:** `org_budget_settle` with realized-so-far.
P2b owns calling these around `/v1/llm/egress`; P4a owns the functions + their guarantees.
