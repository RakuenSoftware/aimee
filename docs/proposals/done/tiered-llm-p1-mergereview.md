# P1 — merge-readiness review (tiered-llm-p1-tenancy-identity)

- **State:** DONE — archived review artifact for the completed P1 delivery.

Branch `worktree-claude+tiered-llm-p1-tenancy` off `testing`: 25 commits, 57 files,
~5.3k insertions. All objects compile `-Werror`; unit tests pass; the DB-layer
behaviors are validated on **real PG17 + pgvector** (a CT matching CI's sidecar).
Requesting a merge-readiness verdict + the concrete remaining blockers.

## Delivered (by proposal invariant)

**Slice 1 — schema + Postgres hardening (I1–I5, I10, I12 substrate).** Hardened
through 4 prior roundtable rounds.
- `kb_team/kb_project/kb_team_membership/kb_project_membership/kb_admin_grant/
  kb_oidc_jwks` + enrollment `(cert_issuer,cert_serial_norm)` + audit identity cols.
- Three-phase provisioning: `schema_roles.sql` (create) → `schema.sql` (DDL, role-free
  so dev-safe) → `schema_grants.sql` (runtime grants). Runtime role non-owner,
  `NOBYPASSRLS`, no-DDL.
- RLS: `FORCE` on all tenant tables; read policies membership-bound + `aimee.team`
  single-team scoping + project `restricted`/`team-open`; own-rows bootstrap policy;
  **admin-gated write policies** (`kb_principal_is_admin()`, bootstrap-owner flag set
  only by `set_tenant_context` from the verified `owner`). `set_tenant_context`
  SECURITY DEFINER validates membership. **Gate `run-p1-rls-gate.sh` (mandatory,
  non-skipping in CI) proves on real PG17**: fail-closed no-context, per-principal
  isolation, non-member reject, GUC-spoof denied, runtime `NOBYPASSRLS`, no
  self-enroll/self-grant, restricted-project invisibility, and the owner→admin→
  non-admin write ladder.
- `db2_init` hardened boot-asserts (verify-full via `PQconninfoParse`; runtime role
  lacks BYPASSRLS/super/CREATE, membership in any table-owning/super role, table
  ownership) — gated by `AIMEE_KB_HARDENED`.

**Slice 2 — identity + revocation.** `kb_identity` (verifier-only principal handle,
**injective** percent-encoded `identity_key`, no-truncation, control-char reject);
`kb_identity_combine` (fail-closed transport∩actor rule, 11-case test) +
`kb_identity_resolve` (DB bootstrap-read + combine); mTLS `kb_tls_peer_issuer/serial`;
`db2_enrollment_is_revoked_by_key` (per-request-from-primary) + eager backfill —
PG-validated; **B5** `X-Aimee-*` ingress deny (both handlers, 8-case test).

**Slice 3 — OIDC hardening.** `iat` token-age ceiling (I9, 7-case test, reject-before-
subtract); **fleet-wide Postgres JWKS** (I10) with bounded 300s refresh, resolver-hook
decoupling, rotation convergence validated on PG.

**Slice 4 — routes + authz (partial).** `/v1/team` + `/v1/project` + `/v1/team/member`
routes (`kb_http_team.c`); the router builds the authenticated **actor** principal
(OIDC issuer-scoped or unscoped-owner) into a per-request thread-local (`kb_reqctx`);
writes admin-gated at the DB layer (403 for non-admins). `kb-v1-coverage` green.

## Round-1 merge-review findings — ALL ADDRESSED (32 commits)

**Security/correctness defects (fixed + re-validated on real PG17):**
- **bootstrap-owner GUC** (a 2nd caller-writable admin bit) removed; admin-ness now
  derives from `aimee.principal='owner'` alone — one trust boundary.
- **auth-off owner-actor manufacturing** dropped — tenancy mutations require a real
  authenticated principal (no anonymous admin writes).
- `identity_key` over-long **rejected** (was truncated to 255 → different principal);
  `db2_tenant_scope_commit()` result **checked** (was ignored → false 200); `access_mode`
  **validated** + DB `CHECK`; OIDC actor only when issuer resolves; strict `?team=` parse;
  per-request actor **cleared at request exit** in both connection handlers.

**The 4 "remaining" items — resolved:**
1. **CLI** ✓ — `aimee-kb team {create,list,add-member,remove-member}` + `aimee-kb project
   {create,list}` (operator CLI on the kb host, in-process db2 as the owner principal).
   **Proven E2E on real PG17.** (The *remote* thin-client `aimee team` needs human-actor→kb
   forwarding — genuinely P5 territory; noted as a P5 follow-up, not a P1 gap.)
2. **OpenAPI** ✓ — `/team`, `/team/member`, `/project` documented; `api-conformance-check`
   green (all documented endpoints routed).
3. **kb-console ACL** ✓ — the routes are actor-gated (not console-admin-scoped), so the
   OIDC console reaches them with its own token; no `acl.go`/`kb_route_acl.c` entry needed.
4. **HTTP integration test** ✓ — `test_team_routes` drives `kb_http_route_ex` E2E
   (no-auth→401, owner→actor→handler→tenant-guard→503-on-shim, scoped→401, bad-body→400);
   real RLS enforcement proven by the mandatory Postgres RLS gate; the db2 chain proven
   E2E by the CLI.

Local `make unit-tests` green; `clang-format` clean; RLS gate PASSES on real PG17.

## Ask

Final verdict: **APPROVE MERGE to `testing`** / **BLOCK** (list any remaining hard
defect). Per the P5-forwarding note, is the remote thin-client `aimee team` correctly a
P5 follow-up rather than a P1 blocker?
