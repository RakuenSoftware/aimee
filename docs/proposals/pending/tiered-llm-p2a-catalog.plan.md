# P2a implementation plan — org model catalog + entitlement (P2 §1, catalog-only)

Slice P2a of P2 (kb egress authority). Branch off `testing` (P1, P3a, P10 s1/s2/s3b
merged). **Catalog-only — holds NO keys, no egress, no vault.** P7 §0: "No org keys
exist on either tier at this point"; P2a introduces the org model *offering* + entitlement
so the server can later merge a blended roster and P2b can route egress. Explicitly
allowed to precede the P7 key-handling. Testable end-to-end (schema + RLS + API + real-PG),
mirroring P1/P3a.

## Verified substrate

- Route pattern (`kb/http/kb_http_team.c`, P1 s4): `kb_reqctx_actor()` → the verified
  actor; mutations run inside `db2_tenant_scope_begin(actor, team)` → commit/rollback.
- Identity/admin: `kb_identity_resolve` (P1 s2), `kb_principal_is_admin()` (db2/schema.sql,
  keyed on `aimee.principal` + `kb_admin_grant`). Tenant RLS via `set_tenant_context`.
- WORM audit: `db2/kb_audit_worm.c` + `kb_audit_event` triggers (append-only, hash-chain).
- P3a `org_model_pricing` is keyed by `billable_model` — the catalog's model relates to
  pricing but is distinct (catalog = offering + entitlement; pricing = cost). A catalog
  entry MAY reference a `billable_model` for the P3 metering key (nullable link).

## Design decisions

1. **`cred_slot_ref` is a server-invisible opaque reference**, scoped by
   `(org, billing_team, provider, model)` (P2 §1) — NOT a key, NOT a vault slot yet. P2a
   stores it as opaque TEXT set by admin CRUD; it is NEVER returned by
   `/v1/models/entitled` and never leaves kb. P2b resolves it to a vault slot after
   authoritative entitlement resolution. In P2a it is inert metadata.
2. **Catalog vs entitlement split.** `org_model_catalog` = the org's model offerings
   (admin-managed, one row per org model). `org_model_entitlement(model_id, team_id)` =
   which teams may use which models (the tenant-scoped join). A caller sees a model ONLY
   if one of their teams is entitled.
3. **RLS = ENABLE-not-FORCE (P3a-consistent).** Reads go through SECURITY DEFINER
   functions (owner bypasses; the non-owner runtime role's direct table reads are
   tenant-filtered as defense-in-depth). Admin writes via admin-gated definer functions.
   No `cred_slot_ref` in any read surface exposed to the server.
4. **Admin CRUD is WORM-audited before/after** (P2 §1 "not merely console-managed"):
   every catalog/entitlement mutation appends a `kb_audit_event` with the before/after,
   the actor, tenant scope; cross-org identifiers rejected; org-admin capability
   (`kb_principal_is_admin`) checked on the primary for every mutation.

## Scope (P2a)

1. **DB2 schema** (`db2/schema.sql` + sqlite mirror + grants):
   - `org_model_catalog(id, model_id TEXT UNIQUE, display_name, provider, wire, billable_model
     TEXT NULL, cred_slot_ref TEXT DEFAULT '', enabled BOOL DEFAULT true, created_at,
     updated_at)`. `wire ∈ {anthropic, openai, responses, gemini}` CHECK.
   - `org_model_entitlement(id, model_id TEXT REFERENCES org_model_catalog(model_id) ON
     DELETE CASCADE, team_id BIGINT REFERENCES kb_team(id) ON DELETE CASCADE, created_at,
     UNIQUE(model_id, team_id))`.
   - ENABLE RLS; admin-only direct read on catalog (cred_slot_ref never in a tenant read);
     entitlement tenant read (team ∈ caller memberships) + admin. SECURITY DEFINER fns:
     `org_catalog_entitled(p_principal) -> rows a caller may use` (join filtered by
     membership; returns model_id/display_name/provider/wire/billable_model — NEVER
     cred_slot_ref); admin-gated `org_catalog_upsert`, `org_catalog_remove`,
     `org_model_entitle`, `org_model_unentitle` (advisory-locked where needed, WORM-audited
     via a kb_audit append inside the txn).
2. **`GET /v1/models/entitled`** (`kb/http/kb_http_models.c`, new): actor from
   `kb_reqctx`, tenant scope, returns the caller's entitled models as JSON (no
   cred_slot_ref, no keys). Wired into the kb router + the OpenAPI/v1 route descriptor.
3. **Admin CRUD** `/v1/models/org/{add,remove,set}` + `/v1/models/org/entitle|unentitle`:
   org-admin gated, tenant-scoped, WORM-audited, cross-org rejected.
4. **CLI** `aimee-kb models org {add,remove,list,entitle,unentitle}` (operator, in-process
   db2 as owner) — mirrors the P1 `aimee-kb team/project` CLI (`kb_main.c`).
5. **Tests**: unit (entitlement resolution excludes non-entitled; cred_slot_ref never in
   the entitled surface; admin gate rejects non-admin); real-PG RLS gate
   (`scripts/p2a_catalog_rls_test.sql`, p3a-style): cross-team entitlement isolation (a
   caller in team A sees only A's entitled models, not B's), admin-only catalog read,
   WORM audit row appended on a mutation. Wired into `run-p1-rls-gate.sh`.

## Explicitly deferred (P2b and later)

The kb egress endpoint `/v1/llm/egress` (§3), the server blended-roster merge (§2), server
routing (§4), the org-key attach (needs the P7 vault key-path), the `org_token_audit`
attribution write on a live call (P2b), budget/rate caps (P4), streaming probes. P2a ships
NO egress and NO keys.

## Gate

- `make -j kb` + `make -j server` link clean; `make lint` (kb-target-isolation,
  v1-route-order, api-conformance, module-boundary) + `make schema-sync-check` green.
- Unit tests + the real-PG p2a catalog RLS gate pass on CT103 (cross-team entitlement
  isolation; cred_slot_ref never exposed; WORM audit on mutation). Existing gates unchanged.
- `/v1/models/entitled` present in the OpenAPI/v1 descriptor (docs-gen / v1-method-coverage
  green).

## Non-goals (P2a)

No egress, no vault, no org keys, no server-side roster merge, no budget. cred_slot_ref is
inert. Pure catalog + entitlement + admin CRUD, tenant-isolated + WORM-audited.
