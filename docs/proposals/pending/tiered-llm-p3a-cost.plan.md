# P3a implementation plan — cost-attribution schema + pricing (the P3 schema half)

Plan for the **P3a** slice of `tiered-llm-p3-cost-attribution.md` (schema +
authoritative pricing; the org-row WRITE rides with P2b, the READ/rollup surfaces
are P3b). Branch off `testing` (P1 already merged). Reuses P1's proven tenancy
substrate (FORCE RLS, three-role split, `set_tenant_context`, `kb_principal_is_admin`,
membership-bound policies) — this is an additive schema packet, no new auth model.

## Scope (P3a only)

1. **DB2 `org_model_pricing`** — Postgres-authoritative, versioned price table:
   `(id, provider, model, input_usd_per_mtok, output_usd_per_mtok,
   cache_read_usd_per_mtok, version, effective_at, created_at)`, unique
   `(model, version)`. A per-model `current_version` pointer table
   `org_model_pricing_current(model, version)` advanced **atomically** on a price
   change (new row + pointer update in one txn) — a price change is a NEW row, never
   an in-place mutate, so every stateless kb instance agrees on `(model, version)`.
   Seeded by promoting the existing static/registry/DB1 `model_pricing` prices.
2. **DB2 `org_token_audit`** — per-request org cost ledger: `(id, request_id,
   origin_cert_cn, actor_issuer, actor_subject, team_id, project_id, model,
   served_model, prompt_tokens, completion_tokens, cache_read_tokens,
   estimated_cost_usd, pricing_version, state, created_at, settled_at)`. Idempotency
   key **unique `(origin_cert_cn, request_id)`** (a bare request_id can replay across
   mutually-untrusted origins). `pricing_version` is a **FK to
   `org_model_pricing(model, version)`** (settlement references the exact immutable
   row). Lifecycle `state ∈ {started, settled_success, settled_denied,
   settled_failed, indeterminate}`.
3. **`(team, project, model, day)` rollup table** — `org_spend_rollup`, maintained
   incrementally (P3b writes it; P3a defines the table + the upsert contends only
   per-key).
4. **RLS (invariant #10):** all three are tenant tables — `FORCE ROW LEVEL
   SECURITY`, membership-bound read policies keyed on `team_id ∈ principal's teams`
   (reuse the P1 `set_tenant_context`/`aimee.principal` pattern), admin/team-lead
   write via P3b. `org_model_pricing` is org-global read (all authenticated), admin
   write.
5. **Typed db2 modules:** `src/db2/org_pricing.{h,c}` (get_current_price(model) →
   pins version; add_price_version; seed), `src/db2/org_token_audit.{h,c}`
   (insert_started, settle, get by key), each guarded by `db2_tenant_require_pg()`.
6. **Cost calc:** `org_token_estimate_cost(model, version, tokens…) → usd` computed
   from the pinned `org_model_pricing` row (never the server's 3-tier resolver — that
   stays for personal DB1 pricing only).
7. **Tests:** unit (pricing version pin, cost calc, idempotency key); the RLS gate
   extended to prove cross-team `org_token_audit` read isolation on real PG17.

## Decisions for the panel

1. **Version-pin mechanics.** `org_model_pricing_current(model)` holds the active
   version; `get_current_price(model)` reads it + the row atomically (single txn,
   primary read per invariant #9) and returns `(version, prices)`; the caller pins
   that version for the whole call. A price change inserts a new
   `(model, version+1)` row then updates the pointer — both in one txn. Sound, or
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
   exists yet — or defer the write API to P2b?
