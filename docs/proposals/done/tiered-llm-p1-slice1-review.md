# P1 Slice 1 — review request (tenancy schema + Postgres hardening)

- **State:** DONE — archived review artifact for the completed slice.

Branch `worktree-claude+tiered-llm-p1-tenancy`, 6 commits, ~2329 lines. This is the
first slice of the converged P1 plan. Requesting roundtable approval to proceed to
slice 2 (identity resolution). The full diff is attached as context.

## What this slice delivers (with verification)

**Schema + Postgres hardening (I1–I5, I10, I12 substrate).**
- `kb_team`, `kb_project`, `kb_team_membership`, `kb_project_membership`,
  `kb_admin_grant`, `kb_oidc_jwks`; enrollment `(cert_issuer, cert_serial_norm)`
  columns; audit identity columns (`actor_issuer/actor_subject/transport_cn/
  team_id/selected_default_from`).
- **Three-phase provisioning** (fixes an ordering bug found in testing):
  `schema_roles.sql` (create roles) → `schema.sql` (DDL, role-free so dev-safe) →
  `schema_grants.sql` (runtime DML/EXECUTE grants). Runtime role is non-owner,
  `NOBYPASSRLS`, no-CREATE.
- **RLS trust model:** `FORCE ROW LEVEL SECURITY`; tenant-data policies bound to
  the principal's own memberships (a wrong/forged `aimee.team` cannot widen
  access); own-rows bootstrap policy on identity tables; `set_tenant_context`
  SECURITY DEFINER validates `team ∈ principal memberships` (needs no BYPASSRLS),
  EXECUTE to runtime only.
- **Verified on real PG17 + pgvector** (not the shim): the full 1825-line
  `schema.sql` applies with the tenancy block integrated, and the mandatory
  isolation gate passes — fail-closed with no context, per-principal isolation,
  non-member team reject, GUC-spoof denied, runtime `NOBYPASSRLS`.

**C tenancy core.**
- `kb_identity` — verifier-only authenticated `kb_principal_t` handle + canonical
  immutable `identity_key` (`oidc:/cert:/owner`) + cert-serial normalization (I5).
- `db2_tenant` — `db2_tenant_scope_begin/_commit/_rollback`: the sole runtime path
  that sets tenant GUCs, via `set_tenant_context` inside a txn, fail-closed `RESET`
  on every path (I4); `db2_tenant_require_pg()` shim hard-fail (B1).
- 5 typed CRUD modules (team/project/membership/admin_grant/oidc_jwks), each
  guarded.
- `db2_hardening` — hardened-tier `db2_init` boot asserts: verify-full DSN (I1) +
  runtime-role privilege floor (B4), gated by `AIMEE_KB_HARDENED` so the existing
  plaintext dev/CI lanes are unchanged.

**Tests / gates (all green).**
- `unit-test-kb-identity`, `unit-test-db2-hardening`, `unit-test-kb-tenancy-shim-guard`
  (all 18 tenant entries hard-fail on the shim — B1/N1) via `make unit-tests`.
- `run-p1-rls-gate.sh` wired into `ci.yml` as a **mandatory, non-skipping** step on
  the pgvector sidecar (the cross-team-deny AC — only provable on real Postgres).
- `check-p1-tenant-guard.py` (N1) in `make lint`: build fails if a new tenant entry
  omits the guard (verified falsifiable).

## Round-4 guard mapping (N1–N6)

- **N1** ✓ shim-guard unit test over all entries + build-time coverage check.
- **N4 (B4)** ✓ `db2_init` boot-asserts runtime role lacks BYPASSRLS/super/CREATE;
  `set_tenant_context` EXECUTE to runtime only (not PUBLIC).
- **N2** — `kb_principal_t` is verifier-only (no raw-string path); `set_tenant_context`
  validates membership internally. (Call-site enforcement lands with slice 2's
  `kb_identity_resolve`.)
- **N3 (B3 backup key), N5 (B5 ingress header-deny)** — see re-scoping below.

## Proposed re-scoping (flagging for the panel)

Two round-4 items are better placed in adjacent slices, and I want the panel's
agreement:
- **B5 (X-Aimee-\* ingress header-deny)** → **slice 2**. It is the rule "actor
  identity comes only from a verified JWT, never a request header," which lives in
  `kb_identity_resolve` (the actor-derivation path). There is no identity-resolution
  ingress to guard until slice 2 exists; adding a header strip now would guard
  nothing.
- **B3/I11 (encrypted-backup key custody + independence)** → **the vault work
  (P10/P7)**, which in the chosen recommended order lands *before* P2 anyway. The
  backup-key custody seam is the same custody abstraction as the vault; building it
  against the vault module (P10) is cleaner than a standalone stub here.

Is this re-scoping acceptable, or does the panel require B5/B3 to land inside
slice 1? Otherwise: is slice 1 sound to merge-forward and proceed to slice 2?
