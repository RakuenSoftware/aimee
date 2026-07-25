# Proposal: Per-user `remote_writes` authorization

- **State:** DRAFT — 2026-07-25; awaiting roundtable review. No implementation has started.
- **Charter roles:** Enforce / Constrain-Verify / Gate-Promote.
- **Thesis:** The `/v1` write gate is already parameterized on a `remote_writes` tier; today that tier
  is one process-global value derived from `aimee.api.remote_writes` and applied to every TCP caller
  behind a single shared bearer. Make the tier a function of the **authenticated individual KB user**
  instead — resolved from an OIDC subject when OIDC is enabled, otherwise from a per-user bearer — and
  source each user's tier from the existing per-(server, team) management config extended to per-user.
  Reuse the existing gate, the existing OIDC verifier, the existing enrollment credential primitives,
  and the existing management-config projection. Add no new policy object, approval vocabulary, or
  audit family.

## 1. Problem

`remote_writes` (`off` | `data` | `full`) is a single global server setting. Enforcement lives in
`server_http_route_allowed_caps` (`src/server/server_http.c`): data-plane writes (the ops in
`g_v1_write_ops`) open at `data`; privileged/exec routes need `full`; everything is authenticated by
**one shared bearer** (plus `scope:` bearers, which are read-only). Two callers presenting the same
bearer are indistinguishable, so a deployment cannot say "user A may write, user B may only read"
without standing up a second server. The tier is a per-appliance switch, not a per-principal grant.

The pieces for per-principal authorization already exist but are not wired to this gate:

- `kb_oidc_verify_jwt()` (`src/kb/auth_oidc.c`) verifies a JWT and yields a `subject`.
- `enroll.c` mints opaque credentials (256-bit, `sha256`-at-rest, constant-time checks).
- `server_mgmt_read_project_config()` / `read_config_projection_valid()` already project a
  per-(server, team) config that **includes `remote_writes`**.
- `kb_mgmt_token` already models per-`subject` capabilities within a `team_id`.

## 2. Design

Per request on the `/v1` TCP path (mTLS remains transport-only), resolve the caller's identity, look
up their write tier, and feed that tier into the **unchanged** gate.

The load-bearing observation: `server_http_effective_conn_caps()` and
`server_http_route_allowed_caps()` already take `remote_writes` as a parameter. At the request seam
(`server_http.c`, ~L1687) the code passes the process-global `g_remote_writes`. The whole behavior
change is to pass a **per-request, per-user** tier there instead. The gate's decision logic — which
ops are data-writes, which need `full` — does not change.

## 3. Identity resolution (new `server_request_identity` unit)

1. **mTLS** authenticates the transport only (unchanged); it does not select the tier.
2. If **OIDC is enabled**, verify the presented JWT via `kb_oidc_verify_jwt()` → `subject` (primary).
3. Otherwise, validate a **per-user bearer** minted through `enroll.c`, bound to `{subject, team}`.
4. No credential match → identity = none.

Output: `{ subject, team, source ∈ {oidc, bearer, none} }`.

## 4. Tier storage & administration

Extend the existing per-(server, team) management config (`server_mgmt_read_project_config` /
`read_config_projection_valid`, backed by the db2 management schema) to carry **subject-keyed**
`remote_writes` entries within a `(server, team)`. A user's tier is administered through the existing
management API — no new policy surface. The server resolves `(server_id, team, subject) → tier` and
caches it in memory, refreshed from the management plane.

## 5. Enforcement & the global setting

- The resolved per-user tier is passed into the gate at the `server_http.c` request seam in place of
  `g_remote_writes`.
- **Per-user fully replaces the global.** `aimee.api.remote_writes` no longer authorizes requests. It
  is initially retained as parsed-but-non-authorizing (telemetry / back-compat) and slated for
  deletion in a later cleanup; `server_mgmt_read_source.c` and the startup status line are updated.
- **Fail closed:** an unmatched or anonymous identity resolves to `off` (writes denied).

## 6. Migration / back-compat

Existing single-bearer deployments must not lock themselves out. On upgrade, the current global tier
seeds a default/admin user grant (or a documented default-principal tier) so a deployment that has not
yet provisioned per-user grants keeps working at its prior tier until an operator differentiates users.

## 7. Phased implementation (one PR per slice, off `testing`, CI-green, never pushed to `testing`)

1. This proposal (review gate).
2. **Identity seam** — `server_request_identity_resolve()`; OIDC-verify + per-user-bearer validation.
   No behavior change (tier still global). Unit tests.
3. **Per-user tier storage + lookup** — extend the mgmt config projection to per-user; lookup + cache;
   admin set/get; db2 migration. Tests.
4. **Gate rewire + retire global** — feed the per-user tier into the gate; fail-closed default; stop
   `aimee.api.remote_writes` authorizing. Tests.
5. **e2e + governance + docs** — extend the local-stack config-mode matrix to assert per-user tiers on
   both the OIDC and per-user-bearer paths; docs; `make lint` (incl. D7) + `make docs-gen`.

## 8. Security considerations

- **Fail closed** on every unresolved identity; never widen on ambiguity.
- **mTLS unchanged** — separating transport from authorization keeps the connection guarantee intact.
- **Constant-time** credential comparison and `sha256`-at-rest for per-user bearers (reuse `enroll.c`).
- **Cache coherence** — a revoked or downgraded grant must take effect promptly; the tier cache carries
  a bounded TTL / invalidation from the management plane so a downgrade is not indefinitely stale.
- **Audit** — write decisions remain observable through the existing governed-action audit bus; no new
  audit vocabulary is introduced.

## 9. Acceptance criteria

- With OIDC enabled, two subjects with tiers `data` and `off` produce `memory.store` → `2xx` and `403`
  respectively; both reads succeed.
- With OIDC disabled, the same holds via per-user bearers.
- An unmatched credential is denied all writes (fail closed) and the global `aimee.api.remote_writes`
  value no longer affects the decision.
- `make lint` (incl. D7 + governance) and `make docs-gen-check` stay green.

## 10. Out of scope

Read-tier partitioning, per-route custom grants beyond the `off`/`data`/`full` tiers, changes to mTLS
transport policy, and the KB's own internal tenancy enforcement (this proposal governs the
aimee-server `/v1` write gate). These remain with their current owners.
