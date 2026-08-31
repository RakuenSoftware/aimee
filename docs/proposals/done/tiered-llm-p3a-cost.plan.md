# P3a implementation plan: cost-attribution schema + pricing (the P3 schema half)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Plan for the **P3a** slice of `tiered-llm-p3-cost-attribution.md` (schema +
authoritative pricing; the org-row WRITE rides with P2b, the READ/rollup surfaces
are P3b). Branch off `testing` (P1 already merged). Reuses P1's proven tenancy
substrate (FORCE RLS, three-role split, `set_tenant_context`, `kb_principal_is_admin`,
membership-bound policies). This is an additive schema packet, no new auth model.

## Scope (P3a only)

1. **DB2 `org_model_pricing`**: Postgres-authoritative, versioned price table:
   `(id, provider, model, input_usd_per_mtok, output_usd_per_mtok,
   cache_read_usd_per_mtok, version, effective_at, created_at)`, unique
   `(model, version)`. A per-model `current_version` pointer table
   `org_model_pricing_current(model, version)` advanced **atomically** on a price
   change (new row + pointer update in one txn). A price change is a NEW row, never
   an in-place mutate, so every stateless kb instance agrees on `(model, version)`.
   Seeded by promoting the existing static/registry/DB1 `model_pricing` prices.
2. **DB2 `org_token_audit`**: per-request org cost ledger: `(id, request_id,
   origin_cert_cn, actor_issuer, actor_subject, team_id, project_id, model,
   served_model, prompt_tokens, completion_tokens, cache_read_tokens,
   estimated_cost_usd, pricing_version, state, created_at, settled_at)`. Idempotency
   key **unique `(origin_cert_cn, request_id)`** (a bare request_id can replay across
   mutually-untrusted origins). `pricing_version` is a **FK to
   `org_model_pricing(model, version)`** (settlement references the exact immutable
   row). Lifecycle `state ∈ {started, settled_success, settled_denied,
   settled_failed, indeterminate}`.
3. **`(team, project, model, day)` rollup table**: `org_spend_rollup`, maintained
   incrementally (P3b writes it; P3a defines the table + the upsert contends only
   per-key).
4. **RLS (invariant #10):** all three are tenant tables. `FORCE ROW LEVEL
   SECURITY`, membership-bound read policies keyed on `team_id ∈ principal's teams`
   (reuse the P1 `set_tenant_context`/`aimee.principal` pattern), admin/team-lead
   write via P3b. `org_model_pricing` is org-global read (all authenticated), admin
   write.
5. **Typed db2 modules:** `src/modules/db2/c/org_pricing.{h,c}` (get_current_price(model) →
   pins version; add_price_version; seed), `src/modules/db2/c/org_token_audit.{h,c}`
   (insert_started, settle, get by key), each guarded by `db2_tenant_require_pg()`.
6. **Cost calc:** `org_token_estimate_cost(model, version, tokens…) → usd` computed
   from the pinned `org_model_pricing` row (never the server's 3-tier resolver; that
   stays for personal DB1 pricing only).
7. **Tests:** unit (pricing version pin, cost calc, idempotency key); the RLS gate
   extended to prove cross-team `org_token_audit` read isolation on real PG17.

## v2 resolutions (round-1 panel)

- **R1, Pricing is NOT org-global-read.** `org_model_pricing` read is restricted to
  **org-admins** and the **egress metering path** (a dedicated `set_config`-gated read
  or SECURITY DEFINER cost function), never all-authenticated (negotiated prices are
  a competitive signal). Team-leads/members never read the price table. Pricing is
  **platform-scoped** in P3a (one org per kb today); a per-org price dimension is a
  documented non-goal here.
- **R2, Pricing rows are IMMUTABLE.** WORM triggers on `org_model_pricing` forbid
  UPDATE/DELETE, so the `pricing_version` FK genuinely pins the price. A price change =
  a NEW `(billable_model, version+1)` row; unique `(billable_model, version)`; version
  is monotonic per model. `org_model_pricing_current(billable_model PK, version)` is
  advanced in the SAME txn as the new row insert, under `SELECT … FOR UPDATE` on the
  pointer row so two concurrent admins cannot fork a version.
- **R3. Seed from a FIXED static list in the repo** (`org_pricing_seed.sql` /
  a checked-in table), NOT the server's DB1 `model_pricing` (crosses the tier
  boundary, non-reproducible across environments). Deterministic version-1 seed.
- **R4, `org_token_audit` is a WORM ledger.** Triggers forbid DELETE and forbid
  UPDATE of the immutable columns `(origin_cert_cn, request_id, team_id, project_id,
  billable_model, pricing_version, actor_issuer, actor_subject)`; the ONLY legal
  UPDATE is a state transition `started → settled_success|settled_denied|
  settled_failed|indeterminate` plus setting `settled_at` + realized tokens/cost, and
  `indeterminate → settled_*` (downward reconciliation). A second settle of an
  already-terminal row is rejected. Unique `(origin_cert_cn, request_id)`; a replay
  whose immutable triple `(team_id, project_id, pricing_version)` differs from the
  bound row is **rejected**, not accepted.
- **R5. Writes come ONLY from the trusted egress path.** No human-principal (admin or
  team-lead) write policy on `org_token_audit`/`org_spend_rollup`. The write/settle
  API is a SECURITY DEFINER function owned by a dedicated writer role, callable only by
  the egress metering path; team-leads/admins are READERS. Reads are gated
  **`org_admin OR team_lead_of(requested_team)`**. P3a adds a `kb_team_lead(identity_key,
  team)` grant (P1-style, RLS-constrained) + an `is_team_lead(team)` predicate.
- **R6. Full columns + constraints.** `org_token_audit`: request_id (NOT NULL),
  origin_cert_cn (NOT NULL), actor_issuer/actor_subject (nullable; set only when a
  kb-verified actor token was present), team_id (FK kb_team, NOT NULL), project_id (FK
  kb_project, nullable), **billable_model** (the `token_billable_model` result, not the
  requested/agent name) + served_model, prompt/completion/cache_read/cache_write_tokens
  (each `CHECK >= 0`), estimated_cost_usd `NUMERIC(20,10)` (exact decimal, not REAL),
  pricing_version FK `(billable_model, version) → org_model_pricing`, session_id,
  delegation_id, state, created_at, settled_at. **project→team consistency** enforced
  (a project's parent must equal the row's team_id) via a trigger/subquery check.
- **R7. Rollup fully specified + same-txn.** `org_spend_rollup(team_id, project_id,
  billable_model, day, prompt_tokens, completion_tokens, cache_read_tokens,
  cost_usd NUMERIC(20,10), row_count, updated_at)`, unique
  `(team_id, COALESCE(project_id,0), billable_model, day)` (null-safe). The **settle API
  upserts the rollup delta in the SAME DB2 transaction** as the ledger settle (`ON
  CONFLICT … DO UPDATE SET prompt_tokens = rollup+excluded, …, row_count+1`), so the
  rollup is always consistent with the ledger. (This is why P3a (not P3b) owns the
  write/settle API.)
- **R8. P3a ships the write/settle + pricing API** (`db2_org_audit_start`,
  `db2_org_audit_settle` [updates rollup in-txn], `db2_org_pricing_current(billable_model)
  → {version, prices}`, `db2_org_pricing_add_version`, `db2_org_pricing_seed`,
  `org_token_estimate_cost(billable_model, version, prompt, completion, cache_read,
  cache_write) → NUMERIC`). Not dead code. It is the schema's contract that P2b calls.
- **R9. Primary-consistency (invariant #9) + guard semantics.** Access-gating reads
  (membership/team-lead/pricing/revocation) use the primary connection (`db2_conn`, the
  primary in the single-primary deployment; a replica-routing selector for reporting
  reads is a documented future item, same posture as P1). `db2_tenant_require_pg()`
  means "Postgres backend required (RLS can't run on the shim)"; `get_current_price`
  needs the PG guard but no tenant-context (it is platform-scoped), documented at the
  call.
- **R10. Tests.** Unit: pricing version-pin + immutability (UPDATE rejected), cost calc
  incl. cache tokens, idempotency-key **replay across different origin CNs rejected**,
  replay with changed immutable triple rejected, WORM (DELETE + double-settle rejected),
  billable-vs-served model. RLS gate extended: `org_token_audit` cross-team read denied;
  org_admin sees all, team_lead sees only their team, member/other-team denied.

## Decisions for the panel (resolved above; confirming)

1. **Version-pin mechanics.** `org_model_pricing_current(model)` holds the active
   version; `get_current_price(model)` reads it + the row atomically (single txn,
   primary read per invariant #9) and returns `(version, prices)`; the caller pins
   that version for the whole call. A price change inserts a new
   `(model, version+1)` row then updates the pointer, both in one txn. Sound, or
   prefer an `effective_at`-window lookup (pick the row whose window covers `now`)?
2. **Seed source.** Promote the server's DB1 `model_pricing` + the static/registry
   defaults into `org_model_pricing` as version 1 at migration. Acceptable to seed
   from the DB1 table's current contents (read once at provisioning), or should the
   seed be a fixed static list checked into the repo?
3. **org_token_audit as WORM-ish?** It's an append-then-settle ledger (state
   transitions started→settled). Should it get WORM triggers (like `kb_audit_event`)
   forbidding row DELETE, allowing only the settle UPDATE, or is that P3b/P7 scope?
4. **P3a/P2b coupling.** The org-row WRITE happens at P2b's egress point (not P3a).
   P3a ships the schema + pricing + the write/settle db2 API, so P2b just calls it.
   Confirm P3a should include the db2 write API (used by P2b) even though no caller
   exists yet, or defer the write API to P2b?

## v3 notes (round-2 panel → baked into implementation; verified in the code diff)

- **Pricing read isolation is a REAL boundary, not set_config.** Team-leads/members
  never touch `org_model_pricing`. The egress path gets cost via a SECURITY DEFINER
  `org_token_estimate_cost(...)` owned by a role that can read pricing; it returns only
  the computed **NUMERIC cost** (never raw prices). Org-admins may read the price table
  via an admin-gated RLS SELECT policy (`kb_principal_is_admin()`). No set_config trust.
- **Immutable vs. settle-writable columns.** IMMUTABLE (trigger-enforced, UPDATE rejected):
  `origin_cert_cn, request_id, team_id, project_id, billable_model, pricing_version,
  actor_issuer, actor_subject, created_at`. WRITABLE ONCE on `started→settled_*` (and
  `indeterminate→settled_*`): `state, settled_at, prompt/completion/cache_read/
  cache_write_tokens, estimated_cost_usd`. DELETE/TRUNCATE forbidden.
- **Version monotonicity under concurrency.** `db2_org_pricing_add_version(model, …)`
  takes `pg_advisory_xact_lock(hashtext('orgprice:'||model))`, computes
  `COALESCE(max(version),0)+1`, inserts the row, and upserts the
  `org_model_pricing_current(billable_model PK, version)` pointer. All in one txn, so
  concurrent admins serialize and versions stay gap-free/monotonic.
- **Idempotency/replay (corrected).** Unique `(origin_cert_cn, request_id)`. A SAME-key
  re-`start` whose immutable triple differs from the bound row → **rejected** (the
  insert is `ON CONFLICT (origin_cert_cn, request_id) DO NOTHING` then a read-back that
  asserts the stored immutable fields match; a mismatch returns a typed conflict). A
  DIFFERENT `origin_cert_cn` with the same `request_id` is a **distinct valid row** (the
  reason the key is composite), NOT a replay. (Corrects the r2 test wording.)
- **project→team consistency** via a `BEFORE INSERT` trigger asserting
  `(SELECT parent FROM kb_project WHERE id = NEW.project_id) = NEW.team_id` when
  project_id is non-null (a plain FK can't express the cross-column rule).
- **kb_team_lead grant.** `kb_team_lead(identity_key, team, granted_at, granted_by)`,
  `identity_key` = the P1 canonical key (`oidc:/cert:/owner`), unique `(identity_key,
  team)`; `is_team_lead(team) = EXISTS(… WHERE identity_key = current_setting('aimee.
  principal') AND team = ?)`. Read policy on the org tables: `kb_principal_is_admin()
  OR is_team_lead(team_id)`. Admin-gated write (like kb_admin_grant), never a human
  write to the ledger.
- **Rollup on terminal settle only.** The rollup delta (`+realized tokens/cost,
  +1 row_count`) is applied once, in the settle txn, on the FIRST transition to a
  terminal `settled_*`. `indeterminate` (P4 reserved-max) and its downward reconciliation
  ride with P4. P3a's rollup upsert is a pure additive delta on realized settle, so no
  double-count. Documented as the P3a boundary.

Plan CONVERGED (panel found no panel-level issues across 2 rounds; residual items are
prove-in-code). Proceeding to implement; the slice diff returns to the roundtable.
