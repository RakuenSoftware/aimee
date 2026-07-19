# P1 — merge-readiness review (tiered-llm-p1-tenancy-identity)

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

## Known remaining (need the panel's ruling on merge-blocking vs fast-follow)

1. **CLI** (`aimee team/project …`) — an explicit AC. The thin-client reaches the
   *server*; kb team-management needs either a server→kb proxy route or a direct-kb
   client path. Which is the intended wiring, and does it block the P1 merge or ride
   as an immediate follow-up on the same branch?
2. **OpenAPI** entries for `/v1/team*` (docs; the coverage gate passes without them).
3. **kb-console ACL** — my routes are actor-gated (not console-admin-scoped), so the
   console reaches them via its OIDC token without ACL changes; confirm no
   `acl.go`/`kb_route_acl.c` entry is required, or add them.
4. **HTTP integration test** (stand up kb + mock JWKS, drive `/v1/team/*` end-to-end).
   The DB layer is proven via the RLS gate and the identity/OIDC logic via unit tests;
   the missing piece is the full request-path E2E. Blocking, or slice-5 fast-follow?

## Ask

Verdict: **MERGE-READY** / **MERGE-AFTER** (list the must-fix blockers) / **BLOCK**.
Please rule specifically on items 1–4 (merge-blocking vs. same-branch fast-follow),
and flag any real security/correctness defect in the delivered code.
