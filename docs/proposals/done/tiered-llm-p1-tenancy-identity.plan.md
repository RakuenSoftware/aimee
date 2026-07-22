# P1 implementation plan (v2) — Tenancy + identity

Plan for `tiered-llm-p1-tenancy-identity.md`. Worktree branch off `testing`.
One feature branch; slices are roundtable checkpoints + commits, merged once at
convergence. **v2 addresses the round-1 panel: every slice carries its own
red/green tests (no tests-only slice); every invariant names its code seam and
its test; nothing load-bearing (fleet JWKS, RLS gate, encrypted backups) is
deferred.**

## Grounding (verified code map)

- **"DB2" = Postgres** via libpq behind a typed boundary (`src/db2/db2.h`); code
  outside `src/db2/` never sees SQL/libpq. One idempotent `src/db2/schema.sql`
  (+ `schema_sqlite.sql` shim, which **cannot enforce RLS**). No versioned
  migrations. RLS/roles/verify-full/`SET LOCAL`/transaction-pooling — greenfield.
- OIDC verifier `src/kb/auth_oidc.c::kb_oidc_verify_jwt` validates `exp`(+60s)
  only — **no `iat`**. JWKS is a per-instance startup file. Enrollments key on
  **`fingerprint`** (`db2/enrollments.c`), storing but not keying on `serial`; no
  `cert_issuer`. kb routes = if/else chain in `kb/http/kb_http.c`; sub-routers per
  module (`kb_http_accounts.c` is the template). Coverage gate
  `check-kb-v1-coverage.py`. CLI: `cmd_*.c` + thin-client `cli_v1_routes_d.c`.
  Console ACL parity: `kb-console/acl.go` + `src/kb/http/kb_route_acl.c` (drift
  test). Per-request connection seam: `db2_lease_begin/_end` (`db2_init.c:465`);
  connection opened in `db2_init` from the `AIMEE_DB2_URL`/`db2_url` DSN.
- Test scaffolding: `test_kb_auth_oidc.c` has real RSA mock-JWKS + RS256 JWT
  minting (`make_jwks`/`make_jwt`) to extend. CMake `aimee_add_test(name srcs)`.

## Cross-cutting invariant → code seam → test (the panel's core ask)

| # | Invariant (proposal) | Code seam | Proof |
|---|---|---|---|
| I1 | verify-full kb↔PG, every kb | `db2_init()` DSN parse — reject a DSN whose effective `sslmode` < `verify-full` unless `AIMEE_DB2_DEV_INSECURE=1` (dev/no-tenant). Not "tenant-holding" — **every** kb. | PG-integration: insecure DSN → boot refused (typed). |
| I2 | 3 roles: owner / migration(DDL) / runtime(DML, non-owner, **no BYPASSRLS**) | DDL (schema apply) runs only under the **migration** DSN at an explicit `aimee-kb migrate` step / bootstrap; the **runtime** DSN (separate credential) drives `db2_init` service start and never issues DDL. Roles created by `schema_roles.sql`. | PG-integration: runtime role attempts DDL → denied; runtime role `rolbypassrls=false` asserted; owner not used at runtime. |
| I3 | Team-scoped RLS on tenant-data; identity-bootstrap policy separate | `FORCE ROW LEVEL SECURITY` + policy `USING (team_id = current_setting('aimee.team',true)::bigint)` on data tables; membership/identity tables get a **bootstrap policy** keyed on `current_setting('aimee.principal',true)` (breaks circularity — resolve teams before a team GUC exists). | PG-integration: cross-team read denied **at DB layer** under runtime role (mandatory, non-skippable CI gate). |
| I4 | Per-request `SET LOCAL` tenant ctx, reset on return, no cross-request leak on pooled conn | New `db2_tenant_scope_begin(principal, team)` / `_end()` wrapping `db2_lease_begin/_end`: opens a txn, `SET LOCAL aimee.principal`, `SET LOCAL aimee.team`; on `_end` (and on **every** error/rollback path) `RESET aimee.team; RESET aimee.principal` before the conn returns to pool. Tenant GUCs enumerated: `aimee.team`, `aimee.principal` (and `aimee.actor` for audit). Correctness independent of PgBouncer mode because each request is its own txn. | PG-integration: two sequential requests reuse one pooled conn; request B sees no leak of A's GUCs; a request that throws mid-txn still clears GUCs. |
| I5 | Immutable revocation key `(cert_issuer, cert_serial)`; CN = policy label | Extend `kb_enrollments` with `cert_issuer`, `cert_serial_norm` (normalized hex, no colons, lowercased); backfill from cert on next handshake; add unique `(cert_issuer, cert_serial_norm)`. Revocation check keys on it; `fingerprint` retained as legacy lookup. Extraction in `server_cert`/`enroll` peer-cert parse. | Unit: normalize fn (leading zeros, case, colon forms); migration backfill; revoke-by-(issuer,serial). |
| I6 | Per-request cert revocation on keep-alive/HTTP-2/pooled mTLS | Re-check seam in the kb request-auth entry (`kb_http` mTLS principal resolution), **per request** after handshake, reading revocation from the **primary** (I8). Not the TLS handshake callback. | Integration: revoke mid keep-alive connection → next request on same conn 401/403. |
| I7 | Composite identity fail-closed | `kb_identity_resolve()` returns `{transport_principal, actor_principal?, method, verified_claims, teams[], default}`. Rule: billing team ∈ (transport.teams ∩ actor.teams) when both present; empty intersection or named-team-not-in-it → **reject (conflicting identity)**; composite default auto-selected **only** if both defaults identical **and** in intersection, else request must name a team. A **server-supplied identity header is never** accepted in place of a kb-verified JWT. | Unit: intersection accept; empty-intersection reject; named-outside reject; differing-defaults→must-name; header-spoof rejected. |
| I8 | Authorization/revocation reads primary-consistent | Auth reads (membership, admin-grant, cert/JWKS revocation) go through `db2_conn_primary()` — a handle pinned to the primary DSN, never the replica routing used by reporting reads. Replica reserved for dashboards/spend. | Integration (2-node PG: primary+replica): membership change visible on next auth read; a forced-stale replica is never consulted for auth (asserted via the primary handle). |
| I9 | OIDC `iat` ceiling | New `oidc_check_token_age(payload, now, max_age, skew)` in `auth_oidc.c`, called from `kb_oidc_verify_jwt` after `exp`: require numeric `iat`; reject missing/malformed; reject `iat > now + skew` (future); **reject `now < iat` before subtracting** (no unsigned underflow); reject `now - iat > max_age` (default 900s) with a typed error regardless of `exp`. | Unit: missing/malformed/future/overflow/`now<iat`/over-age-but-valid-exp. |
| I10 | Fleet-wide JWKS (not per-instance) | New `kb_oidc_jwks` table (issuer, kid, jwk_json, added_at, retired_at); admin write path `POST /v1/oidc/jwks` (org-admin gated) + `aimee oidc jwks add/list/retire`. Verifier resolves keys from PG with a **bounded refresh** (TTL cache, default 300s); the env **file is dev-only fallback used only when no PG JWKS row exists**, and a JWKS-authenticating kb requires the PG source (file cannot reintroduce drift). | Integration: instance A adds a key → instance B (fresh cache) accepts a JWT signed by it within TTL; retire → rejected after TTL; file fallback ignored when PG rows present. |
| I11 | Encrypted backups + WAL + independent key custody + tested restore | `scripts/kb-backup.sh` / `kb-restore.sh`: `pg_dump` piped through envelope encryption with a **backup key from `AIMEE_KB_BACKUP_KEY_SOURCE`** (distinct source from the vault root). CI test: backup → assert output is ciphertext (no plaintext team names) → restore into a clean container → query a seeded row → assert backup key material ≠ vault-root key material. Production selects a KMS-custodied key via the same seam (documented in `docs/dev/kb-backup.md`); CI uses a distinct local key standing in for KMS. | PG-integration CI test (required): encrypt→restore→query→key-distinctness. |
| I12 | Audit of identity resolution | Every `kb_identity_resolve` outcome (accept/reject) writes a `kb_audit_event` row carrying **separate immutable fields** `actor_issuer`, `actor_subject`, `transport_cn`, `verdict`, `team` — never a collapsed `user:<sub>`. Reuses the existing hash-chained `kb_audit_event`. | Unit: resolution emits audit row with separate fields; reject emits reason. |

## Slices (each independently tested; merged on one branch)

**Slice 1 — Tenancy schema + Postgres hardening baseline** (I1–I4, I11, plus the
schema for I5/I10/I12). Tables `kb_team`, `kb_project`, `kb_team_membership`,
`kb_project_membership`, `kb_admin_grant`, `kb_oidc_jwks`, enrollment
`cert_issuer/cert_serial_norm` columns; typed `src/db2/team.*`, `project.*`,
`membership.*`, `admin_grant.*` modules; `schema_roles.sql`; RLS policies +
`FORCE`; `db2_tenant_scope_begin/_end`; `verify-full` enforcement in `db2_init`;
DDL/runtime startup split; backup/restore scripts. **Tests ride with slice:** the
mandatory PG-container integration gate proving I1–I4 + I11 (cross-team deny,
pooled-conn no-leak incl. error paths, concurrent default-team partial-unique,
verify-full refusal, runtime-role no-DDL/no-BYPASSRLS, encrypted backup→restore).

**Slice 2 — Identity resolution + composite identity + revocation + projects**
(I5–I8, I12). `kb_identity_resolve` + authenticated request context; composite
fail-closed rule; `(cert_issuer, cert_serial)` extraction/normalize/migrate;
per-request cert-revocation re-check seam; primary-pinned auth reads; project
authorization (parent==team + entitlement; team-open/restricted; last-member
removal stays restricted = deny, never fail-open; typed errors per failure);
audit rows. **Tests ride with slice:** unit (composite matrix, header-spoof
reject, cert→team, normalize, project authz incl. last-member, audit fields) +
integration (per-request revocation on keep-alive; primary-consistency on a
primary+replica pair).

**Slice 3 — OIDC data-plane hardening + fleet-wide JWKS** (I9, I10). `iat`
ceiling fn; promote verifier to first-class data-plane authenticator resolving
`(iss, sub)` → slice 2; PG-backed fleet JWKS with bounded refresh + admin write
path + dev-file fallback that can't reintroduce drift. **Tests ride with slice:**
unit (`iat` edge matrix; JWT→team valid/expired/wrong-aud/missing-claim;
membership change honored next request via primary re-resolve) + integration
(cross-instance JWKS convergence, retire, file-fallback-ignored-when-PG-present).

**Slice 4 — Routes + CLI + console + org-admin + bootstrap** (§4). `/v1/team/*`
+ `/v1/project/*` (create/list/show/add-member/remove-member) via new
`kb_http_team.c`; `aimee team …` + `aimee project …` (`cmd_team.c`, `cmd_project.c`
+ thin-client routes); org-admin capability with defined source of authority
(canonical OIDC / `cert:CN` / owner principal), RLS-constrained revocable grants,
checked from composite identity on the **primary per request**; **first-team
owner-only bootstrap** (install-time owner identified by the bootstrap owner
principal / initial cert, authorized without a pre-existing team under a
dedicated policy); OpenAPI + `check-kb-v1-coverage.py`; console ACL parity
(per-route entries in **both** `acl.go` and `kb_route_acl.c`). **Tests ride with
slice:** team+project create over **both** CLI and `/v1`; org-admin gating
enforced **server-side** for unauthorized callers (not only console); first-team
bootstrap; named/default team selection (named-outside-set rejected,
absent→default); owner-token resolution; **no-OIDC operation** (certs only);
deny-on-unknown; immediate membership removal honored next request; per-route ACL
drift parity.

## Decisions (now resolved per the round-1 panel)

1. **Ops boundary** → encrypted backup/WAL + restore is **in-scope and CI-tested**
   (I11), not runbook-only; production KMS custody is the same key seam with a
   distinct key, proved distinct in CI.
2. **RLS test** → **mandatory non-skippable** PG-container CI gate exercising the
   actual roles + policies (I3); the suite fails, not skips, without Postgres.
3. **Fleet JWKS** → **in P1** (I10), fully specified + convergence-tested.
4. **Transaction-pooling** → explicit per-request txn + enumerated tenant GUCs +
   fail-closed RESET on all paths + separate bootstrap-principal context (I4);
   DDL/runtime credential split (I2) so kb holds no DDL creds at runtime.
5. **Slice order** → schema first (slice 1) because RLS/roles are the substrate,
   but the **composite-identity contract (slice 2) is fully unit-tested the moment
   it lands** — not deferred to a final slice — so the most security-critical
   logic is verified early.

## CI / testability notes

- New required CTest: `kb_p1_pg_integration` (spins a Postgres container, applies
  `schema_roles.sql` + `schema.sql` under the migration role, runs the service
  under the runtime role) — **fails if Postgres is unavailable** in CI (not
  skipped); a developer without Docker can run the pure-unit subset.
- Primary/replica proofs (I8) use a streaming-replica pair in the same harness.
- Unit tests split by concern into `test_kb_identity.c`, `test_kb_tenancy.c`,
  `test_kb_oidc_iat.c`, `test_kb_jwks_fleet.c` (not piled into
  `test_kb_auth_oidc.c`).

## v3 resolutions (round-2 panel, 29 findings)

**RLS trust model (the two real design fixes).**
- **Bootstrap policy scopes to the principal's OWN rows.** The identity-bootstrap
  policy on `kb_team_membership`/`kb_project_membership` is
  `USING (identity_key = current_setting('aimee.principal'))` — a principal can
  read only *its own* bindings, never all teams'. `aimee.principal` is set **only**
  from the authenticated transport/actor principal, never from caller input.
- **Tenant GUCs are set only via a `SECURITY DEFINER` `set_tenant_context(principal,
  team)`** owned by the owner role, `GRANT EXECUTE` to runtime only; it validates
  `team ∈ (SELECT team FROM kb_team_membership WHERE identity_key = principal)`
  before `SET LOCAL`. **Data-table RLS also binds to principal membership** —
  `USING (team_id IN (SELECT team FROM kb_team_membership WHERE identity_key =
  current_setting('aimee.principal')) AND team_id = current_setting('aimee.team')::bigint)`
  — so a handler that forgets the wrapper or sets a wrong `aimee.team` still cannot
  read another team (defense-in-depth; the GUC only *selects among* the caller's
  own teams). Test: runtime role directly `SET LOCAL aimee.team=<other>` → still 0
  rows.

**Named seams / values (specificity asks).**
- **`kb_identity_resolve` single choke point:** every authenticated request entry
  builds its auth context through one `kb_http_auth_context()`; a request with no
  resolved context is **rejected 401** (fail-closed). Test: a handler reached
  without resolution returns 401, never proceeds.
- **I6 per-request cert revocation:** today the mTLS principal is resolved once at
  handshake and cached on the conn. Fix: the cached result keeps only the
  `(cert_issuer, cert_serial_norm)`; a **per-request** `kb_authz_cert_live()` hook
  re-reads revocation from the **primary** each request. Test: revoke mid keep-alive
  → next request on same conn 403.
- **I5 no revocation window:** migration **eagerly backfills** `cert_issuer`
  (= enrollment CA subject, single issuer) + `cert_serial_norm` for **all active
  enrollments** in one `UPDATE` at schema apply — not lazily at next handshake. During
  transition, revoke also honors `fingerprint`. So there is no key-less window.
- **I8 primary reachability + all call sites:** auth reads go through a typed
  `kb_authz_*` API that internally uses `db2_conn_primary()`; a test enumerates the
  auth call sites and asserts each resolves via the primary handle. Replica routing is
  reserved for `kb_report_*` reads only.
- **I9 skew:** skew = **60s** (reuse the existing `exp` leeway), symmetric; order
  `now < iat - skew` → reject **before** any subtraction; enumerated test values
  (`iat=now+120` reject; `iat=now-60` ok; `iat` missing/`"abc"`/`now-1000` with
  `max_age=900` reject; `now=iat` ok). Overflow-safe on 32- and 64-bit `time_t`.
- **I7 header spoof:** the smuggle vector is a `X-Aimee-Actor`/identity header on the
  **server→kb** channel; kb **ignores** any such header and derives `actor_principal`
  only from a kb-verified JWT (or owner/bearer). Test: server→kb request with a forged
  actor header + no JWT → resolves transport-only, header value never enters context.
- **I10 JWKS TTL vs SLA + hot path:** refresh TTL default **300s, asserted ≤ the
  900s human-cred SLA**; retire is honored within TTL fleet-wide. JWKS uses its **own**
  short-TTL cache (miss → one primary read), **not** a per-request primary read; the
  per-request primary read is the membership authorization check (the authoritative
  source). Test: retired key rejected across instances within TTL.
- **I11 backup key seam:** production key comes from the **same custody abstraction as
  the vault** (`AIMEE_KB_BACKUP_KEY_SOURCE ∈ file|tpm|kms`), resolved/unwrapped at
  backup+restore time, **never stored as host plaintext**; rotation documented. CI uses
  `file` with a key provably distinct from the vault root. I11 is labeled its **own
  operational sub-gate** inside slice 1.
- **I12 audit columns:** `kb_audit_event` today has `actor_principal/verdict/detail/…`
  + WORM triggers + INSERT/SELECT-only writer role. Add via `ALTER … IF NOT EXISTS`:
  `actor_issuer`, `actor_subject`, `transport_cn`, `team_id`, and for composite-default
  `selected_default_from` (which principal's default drove selection) + the resolved
  intersection. Test: accept and reject each emit a row with these separate fields.
- **`AIMEE_DB2_DEV_INSECURE` hardening:** compiled **only in debug builds** and
  **refuses to start if any `kb_team` row exists** — it can never silently downgrade a
  tenant-holding production kb. Test: with a team row present, the insecure flag aborts
  boot.
- **SQLite shim guard:** tenant-scoped db2 calls assert the PG backend
  (`aimee_pg_is_shim()==0`) and hard-fail on the shim, so a developer cannot pass a
  tenant/RLS test on SQLite. Dev/no-tenant (single-tenant) paths still use the shim.
- **First-team bootstrap owner set:** the bootstrap owner is a **configurable set** of
  principals (not one cert), rotatable; losing all is an operator-recovery path
  (re-run install bootstrap) documented in `docs/dev/kb-tenancy.md`.
- **Merge gate:** the branch merges to `testing` **only once**, when **all** slice
  tests across the branch are green and the roundtable has converged — never a
  half-slice on `testing`. So Slice 1's schema columns are never "unused on testing";
  their behaviors (slices 2–3) are on the same branch before it merges.
- **ACL drift test authoritative:** the drift test **fails CI** if any
  `/v1/team/*` or `/v1/project/*` route lacks matching entries in **both** `acl.go`
  and `kb_route_acl.c`; each new route is added to both.
- **I4 immediate-rollback path:** tested cases include commit, explicit rollback,
  exception mid-txn, **and a connection reset/`PQreset` mid-request** — all must clear
  `aimee.team`/`aimee.principal` before the conn re-enters the pool (fail-closed
  `RESET` in the lease-return path, not only on the happy path).
- **last-member race:** concurrent `add-member` vs `remove-member` on a `restricted`
  project's last row is serialized by `SELECT … FOR UPDATE` on the project row; test
  the race resolves deterministically (never leaves the project readable by a
  non-member).

## v4 resolutions (round-3 panel — six enforced-guard blockers)

The panel converged to six blockers, each resolved as a **boot-time/CI-enforced
guard with an enumerated test** (not convention):

- **B1 — Shim guard is tested on every tenant entry.** A required test
  `test_kb_tenancy_shim_guard` calls **each** tenant-scoped db2 entry
  (`db2_tenant_scope_begin`, `kb_identity_resolve`, `kb_team_*`, `kb_project_*`,
  `kb_membership_*`) under the SQLite shim and asserts each returns the typed
  `DB2_ERR_TENANT_REQUIRES_PG` hard-fail. The list is derived from a single
  `TENANT_SCOPED_ENTRIES[]` registry so a new tenant entry that isn't registered
  fails a coverage assertion (no silent bypass).
- **B2 — `set_tenant_context` takes an authenticated handle, not a string.** Its
  signature is `set_tenant_context(const kb_principal_t *authn, int64_t team)`
  where `kb_principal_t` is produced **only** by the verifier
  (`kb_oidc_verify_jwt` / mTLS peer parse) and carries a process-local
  authentication marker; the SQL wrapper is fed the principal key **from that
  struct**, never from a request string. Test `test_kb_identity_authn_binding`
  calls the wrapper with an attacker-controlled principal string (no verifier
  handle) and asserts rejection; a raw-string overload does not exist.
- **B3 — Backup key independence is boot-enforced.** At startup a key-holding kb
  asserts: (a) `AIMEE_KB_BACKUP_KEY_SOURCE` **set** (unset → typed abort);
  (b) resolved backup key material **≠** vault-root key material (equal → abort);
  no hard-coded default. CI test asserts (a), (b), and (c) backup output is
  ciphertext (no plaintext team name present).
- **B4 — Runtime role privilege is boot-asserted.** `db2_init` introspects the
  connected role: refuses boot with a typed error if `rolbypassrls` is true or the
  role holds `CREATE` on the tenant schema (runtime = DML only). Proven in the
  PG-integration gate (over-granted role → boot refused).
- **B5 — Deny-by-default identity-header ingress policy.** A single kb ingress
  filter **drops and logs** any request-supplied `X-Aimee-*` identity header on
  **all** ingress paths (server→kb, direct human, mgmt, internal RPC); actor
  identity derives **only** from a kb-verified JWT / owner-bearer. Test
  `test_kb_ingress_header_deny` enumerates the ingress entry points and asserts
  none consults a request-supplied identity header.
- **B6 — Single-merge gate is an enforced CI check.** A required CI job
  `p1_all_slices_green` runs every enumerated slice test (unit + PG-integration)
  and **fails the merge** if any is absent or red; it is a required status check,
  not advisory. The branch cannot merge to `testing` with any invariant untested.
- **B7 (suggestion) — Reversible backfill.** The eager
  `cert_issuer/cert_serial_norm` backfill has a documented rollback; an integration
  test exercises apply → rollback → reapply idempotently.

## Plan status: CONVERGED (round 4 — panel found no issues)

Round-4 panel returned **no issues**; the 9 residual replay-verifier items are
"prove-in-code" nuances, folded below as **slice-1 acceptance checks verified in
the diff review** (a plan cannot pin them — the code does):

- **N1 (B1):** the tenant-scope **dispatch itself iterates `TENANT_SCOPED_ENTRIES[]`**
  (registry drives runtime, not just the test); a build-time check asserts every
  db2 tenant-scoped function is registered (unregistered entry → build/CI fail).
- **N2 (B2):** `kb_principal_t` is an **opaque, verifier-only** type; `set_tenant_context`
  is its only consumer; the SECURITY DEFINER SQL wrapper validates
  `team_id ∈ principal membership` internally — no raw-string path exists (compile-time).
- **N3 (B3):** the backup-key boot check compares **resolved** key material for **any**
  source (file **and** KMS), so the KMS production path is equally fail-closed.
- **N4 (B4):** boot also asserts `set_tenant_context` EXECUTE is granted **only** to the
  runtime role (never `PUBLIC`).
- **N5 (B5):** kb has **exactly one ingress seam — `kb_http_route_ex`** (all HTTP/mTLS
  requests funnel through it; there is no separate internal RPC). The deny-by-default
  `X-Aimee-*` identity-header filter and its enumerating test target that single seam.
- **N6 (B6):** `p1_all_slices_green` is a **required status check on the PR targeting
  `testing`** (branch protection), not advisory.

**Next action: implement slice 1**, then submit the slice-1 diff to the roundtable
for code-level verification of N1–N6 + the slice-1 tests.
