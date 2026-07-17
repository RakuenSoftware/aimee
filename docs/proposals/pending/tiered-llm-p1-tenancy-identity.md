# Proposal: P1 — Tenancy + identity (teams/projects on OIDC, no virtual keys)

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** nothing. **Blocks:** P2, P3, P4, P5.

## Thesis

Everything else in the series references "which team is this." aimee has no
team/org/project entity today — attribution stops at principal/session ≈ user
(`vault_service.h:74-80` states aimee-server is deliberately single-user). This
packet adds the **tenancy spine** on aimee-kb (the org tier) and binds it to
identity aimee *already verifies*: OIDC subjects for humans, `cert:CN` for
machines. No virtual keys — the OIDC subject *is* the identity (see the master
ADR).

## Goal

On aimee-kb: a `team` (and optional `project`) entity, a mapping from an
authenticated caller to their team(s), and OIDC turned on for the kb data plane so
a human request arrives already resolved to `user → team(s)`.

## §0 What already exists

- **OIDC/JWT verifier** — `src/kb/auth_oidc.c` (`kb_oidc_verify_jwt`, RS256/JWKS,
  alg-pinned, `iss`/`aud`/`exp` checked), wired at `src/kb/kb_main.c:680` via
  `kb_oidc_register_from_env()`, **off unless `AIMEE_KB_OIDC_JWKS_FILE` is set**.
  It already maps a claim → scope.
- **Scope machinery** — `src/headers/kb_scope.h` parses `scope:kind:id:secret` and
  enforces per-`kind:id` isolation (`project:foo`, `workspace:Y`). This is the
  closest existing thing to multi-tenancy; P1 promotes `kind:id` into a first-class
  team/project entity rather than an ad-hoc string.
- **Enrollment identities** — `cert:CN` principals from the mTLS CA
  (`src/db2/enrollments.h` carries `scope[128]`).

## §1 Team/project entities (DB2, kb-owned)

Add `team` and `project` tables in `src/db2/` (behind the KB service — server/CLI
must not touch DB2 directly, per the storage boundary). Minimal columns: `id`,
`name`, `parent` (project → team), `created_at`, `operator_id`. No user table —
users are external identities (OIDC `sub`), not rows we own.

## §2 Identity → team binding

A `team_membership` mapping from an **identity selector** to a team:
- Machine / always-present: match on `cert:CN` (enrollment identity).
- OIDC *(only when configured)*: match on a claim (`sub`, `email`, or a groups
  claim — configurable, since IdPs differ). Reuse the claim-extraction already in
  `kb_oidc_verify_jwt`.
- Owner/bearer: a configured token may map to a default team (covers the no-IdP
  single-org case).
Resolution returns the caller's team set + a default team. This is the single
function P2/P3/P4 call: `kb_identity_resolve(principal) → {teams[], default}` — and
it works identically whether the principal arrived as a cert, a JWT, or the owner
token, so no downstream packet cares whether OIDC is on.

## §3 OIDC on the kb data plane — additive and optional

Promote the existing verifier to a first-class, documented data-plane
authenticator: when configured, a request bearing a valid OIDC JWT is
authenticated as `user:<sub>` and resolved via §2.

**OIDC is never required — it is enabled only when the org configures it on kb**
(the verifier already registers *additively* after the owner token, opt-in via
`AIMEE_KB_OIDC_JWKS_FILE`). When no OIDC is configured:
- `cert:CN` (enrollment) is the always-present identity, and §2 binds teams off it.
- Human callers use the existing owner-token/bearer auth; team binding can map a
  configured owner/bearer to a default team.
- Nothing in §1–§4 (or in P2–P5) depends on OIDC being present — it only *adds* a
  human-SSO identity source. A single-org box with no IdP runs entirely on certs.

No SAML.

## §4 CLI + route surface

- `aimee team {create,list,show,add-member,remove-member}` → new `/v1/team/*` on
  aimee-kb, added to OpenAPI + `v1-method-coverage` (first-class, conformance-
  tested), gated behind an org-admin capability.
- `kb-console`: extend the deny-by-default allowlist (`kb-console/acl.go` +
  `src/kb/http/kb_route_acl.c`) to expose team management in the existing
  OIDC-authenticated console.

## Acceptance criteria

- A team/project can be created and a member added, over CLI and `/v1`.
- An OIDC-authenticated request resolves to `user:<sub>` and its team set; an
  mTLS `cert:CN` request resolves to its team.
- `kb_identity_resolve` returns stable results for OIDC, cert, and owner-token
  callers; unknown identity → empty team set (deny), not a default-admin fallback.
- OpenAPI + coverage green for the new routes; org-admin gating enforced
  server-side (not just in the console).

## Testing

Unit: JWT→team resolution (valid/expired/wrong-aud/missing-claim), cert→team,
membership CRUD, deny-on-unknown. Integration: stand up kb with a mock JWKS, drive
`/v1/team/*` and an authenticated data-plane call end-to-end.

## Non-goals

No virtual keys. No user table (identities are external). No enforcement yet
(budgets are P4). No aimee-server changes — P1 is entirely kb-side.
