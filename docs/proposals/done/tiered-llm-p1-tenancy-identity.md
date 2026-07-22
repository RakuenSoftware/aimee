# Proposal: P1 — Tenancy + identity (teams/projects on OIDC, no virtual keys)

- **State:** proposed (pending — not started). Part of `tiered-llm-offering.md`.
- **Author:** JBailes (drafted by the engineer agent, 2026-07-17).
- **Depends on:** nothing. **Blocks:** P2, P3, P4, P5.

## Thesis

Every subsequent packet in this series assumes "which team is this." aimee has no
team/org/project entity today — attribution stops at principal/session ≈ user
(`vault_service.h:74-80` states aimee-server is deliberately single-user). This
packet adds the **tenancy spine** on aimee-kb (the org tier) and binds it to
the identities aimee already verifies: OIDC subjects for humans, `cert:CN` for
machines. There are no virtual keys — the OIDC subject *is* the identity (see the
master ADR).

## Goal

On aimee-kb, introduce a `team` (and optional `project`) entity, a mapping from
an authenticated caller to their team(s), and OIDC enabled on the kb data plane
so a human request arrives already resolved to `user → team(s)`.

## §0 What already exists

- **OIDC/JWT verifier** — `src/kb/auth_oidc.c` (`kb_oidc_verify_jwt`, RS256/JWKS,
  alg-pinned, `iss`/`aud`/`exp` checked), wired at `src/kb/kb_main.c:680` via
  `kb_oidc_register_from_env()`. **Off unless OIDC is configured.**
  It already maps a claim to scope. At scale the trusted signing-key set must be
  **fleet-wide, not a per-instance `AIMEE_KB_OIDC_JWKS_FILE`**: the JWKS config lives in
  shared Postgres (or a synced source) so all stateless instances agree on which keys are
  trusted and IdP rotation converges within a bounded refresh — never per-instance drift
  where one kb accepts a JWT another rejects.
- **Scope machinery** — `src/headers/kb_scope.h` parses `scope:kind:id:secret` and
  enforces per-`kind:id` isolation (`project:foo`, `workspace:Y`). This is the
  closest existing analog to multi-tenancy; P1 promotes `kind:id` into a
  first-class team/project entity rather than leaving it as an ad-hoc string.
- **Enrollment identities** — `cert:CN` principals from the mTLS CA
  (`src/db2/enrollments.h` carries `scope[128]`).

## §1 Team/project entities (DB2, kb-owned)

Add `team` and `project` tables in `src/db2/` (behind the KB service — server and
CLI must not touch DB2 directly, per the storage boundary). **P1 also establishes
the Postgres hardening baseline (invariant #10)** that every later DB2-adding packet
extends: kb↔Postgres `verify-full` TLS (incl. replicas), separate migration vs.
runtime DB roles (runtime = DML on its own schemas, no DDL/superuser), team-scoped
access predicates / row-level security on tenant tables — with `FORCE ROW LEVEL
SECURITY` and a **non-owner runtime role that lacks `BYPASSRLS`** (so Postgres
owner-bypass cannot defeat the predicate), and **with the authenticated
tenant conveyed per request via `SET LOCAL` / `set_config()` inside the request
transaction (so RLS predicates read it) and reset on connection return, so a pooled
shared runtime role never leaks one request's tenant context into the next (which
requires every tenant-scoped request to run **inside a transaction** and the pool to be
in **transaction-pooling** mode, e.g. PgBouncer, resetting session state on return)**.
**Three distinct DB roles:** a table **owner** role (not used at runtime), a **migration**
role (DDL only, run out-of-band), and the **runtime** role (DML only, non-owner,
no `BYPASSRLS`) — so no runtime path holds owner privileges that would bypass RLS. **DB2
is network-isolated to kb:** the server holds no DB2 route or credential, so the
"server/CLI never touch DB2" boundary is enforced by the network + role grants, not by
convention. And
encrypted backups + WAL, **delivered concretely** (an encrypting backup tool such as
pgBackRest/wal-g with a managed key, or storage-layer/TDE encryption) with a
**tested restore path**. The backup-encryption key is **custodied independently of the
vault root** (a distinct KMS/anchor key), so a stolen backup plus the vault unwrap
anchor still yields no plaintext org key — the two must never share a root. Minimal columns:
`id`, `name`, `parent` (project → team), `created_at`, `operator_id`. There is
no user table; users are external identities (OIDC `sub`), not rows we own.

## §2 Identity → team binding

A `team_membership` mapping from an **identity selector** to a team:
- **Machine / always-present:** match on `cert:CN` (enrollment identity).
- **OIDC** *(only when configured)*: match on an **issuer-scoped stable subject** —
  the identity key is `(iss, sub)`, never a bare `sub` or a mutable `email` (which can
  be reassigned or collide across IdPs); `email`/groups are treated as *attributes*,
  not the identity key. Reuse the claim-extraction already in `kb_oidc_verify_jwt`.
- **Owner/bearer:** a configured token may map to a default team (covers the
  no-IdP single-org case).

The `team_membership` identity key is **canonical and immutable** — `(kind, issuer,
subject)` for OIDC, `(cert_issuer, cert_serial)` for machines (the `cert:CN` is kept
only as the policy label, never the uniqueness/revocation key) — with a **unique
constraint** over `(identity_key, team)` and **at most one `default` per identity_key** (a
**partial unique index** `WHERE is_default`, so racing instances cannot create two
defaults),
enforced in Postgres so racing stateless instances cannot create duplicate or
double-default bindings. **Identity bootstrap avoids the RLS circularity:** the
membership lookup that resolves a caller's teams is constrained by the *authenticated
canonical principal itself*, not a caller-selected team via `SET LOCAL` (which would be
circular — you'd need the team before you could read membership). Team-scoped RLS
applies to tenant *data* tables; identity/membership resolution runs under a separate
identity-bootstrap policy keyed on the authenticated principal.

Resolution returns the caller's team set plus a default team. This is the single
function P2/P3/P4 call: `kb_identity_resolve(principal) → {teams[], default}`.
It behaves identically whether the principal arrived as a cert, a JWT, or the
owner token, so no downstream packet depends on OIDC being on.

**Authorization reads are primary-consistent.** Team membership, entitlement, and
credential/certificate **revocation** status are authorization-sensitive: they are
read from the **primary** (or via a read-your-writes path), never a lagging replica,
so a membership change, entitlement removal, or `cert.revoke` takes effect
immediately (invariant #9). Revocation is keyed by the immutable **`(cert_issuer,
cert_serial)`** (CN is the policy label only) and read from the **primary** revocation
list. Certificate revocation is enforced **per request**, not
only at the TLS handshake: kb re-checks the peer cert's revocation status (from the
primary) on each request, so a persistent keep-alive / HTTP/2 / pooled mTLS connection
stops authorizing the instant the cert is revoked — it cannot keep riding an
already-established handshake. Only non-authorization reporting reads — dashboards,
spend history — may be served from replicas. (How replicas are reached — libpq
routing / PgBouncer / DNS — is a deployment detail; every connection is verify-full
TLS, and authorization/exactness reads pin to the primary regardless of routing.)

**Composite identity contract.** A request carries up to two authenticated
principals, and resolution combines them fail-closed. `kb_identity_resolve` returns
an **authenticated request context** — `{transport_principal, actor_principal?,
method, verified_claims, teams[], default}` — not a single collapsed principal:
- **`transport_principal`** — the mTLS `cert:CN`. Present (and mandatory) on any
  **server-originated** request: all org egress rides a server→kb mTLS channel, so
  those always carry it. A **direct human caller** to kb (console/CLI authenticating
  by OIDC or owner/bearer) may present no client cert; then there is no transport
  principal and the request is authenticated by the token alone.
- **`actor_principal`** — the **issuer-scoped** identity `{kind: oidc, issuer,
  subject}`, kept as **separate immutable fields throughout** (auth, membership, audit,
  policy) — never collapsed to a bare `user:<sub>`, which would let subjects collide
  across IdPs — from an **independently kb-verified** OIDC JWT, or the owner/bearer
  identity; present when a human identity is supplied. A server-supplied identity header
  is **never** accepted in place of a verified JWT.
- **At least one principal is always present.** Every authenticated request is one
  of: transport-only (a server acting for itself), actor-only (a direct human caller
  with no client cert), or both (a server forwarding a kb-verified human identity).
- **Combination rule (fail-closed):** the resolved billing team must be valid for
  **every** principal present — when both a transport and an actor principal are
  present, the billing team must lie in the **intersection** of their team sets, so
  a server cannot bill a team it is not scoped to and an actor cannot bill via a
  server outside that team. If the intersection is empty, or a named team is not in
  it, the request is **rejected** (conflicting identity) — never silently widened or
  defaulted to one side.

**The `default` team is the authoritative billing/attribution team** for any call
that does not name a team. A multi-team caller MAY name a team via a request field
(e.g. `team_id` / `X-Aimee-Team`); kb accepts it only if it is in the caller's
resolved set (and, under the composite rule above, within the transport∩actor
intersection). **Naming a team outside that set is rejected — never silently
downgraded to `default`.** Only an *absent* team falls back to `default`. Entitlement (P2), attribution (P3),
and budget (P4) all key off this single resolved team — there is never an ambiguous
"which of my teams gets charged," and a caller can never bill a team it is not in.
**Composite default:** when both principals are present and the request names no team,
kb auto-selects a `default` **only if the two principals' defaults are identical and in
the intersection**; if their defaults differ, the request **must name a team
explicitly** — kb never silently picks one side's default.

**Project selection is authorized, not merely named.** A `project` belongs to
exactly one team (its `parent`). When a request names a project, kb requires both
(a) the project's `parent` equals the resolved billing team, and (b) the caller is
entitled to it — team membership grants that team's projects by default, unless a
project carries explicit member bindings (a project's **access mode is explicit** —
`team-open` | `restricted`, stored on the project row, so removing the last member of a
`restricted` project leaves it restricted-and-empty (deny), **never fails open** to the
whole team) — a **`project_membership` entity**
`(project_id, identity_key)`, unique, RLS-scoped to the project's parent team, CRUD via
`aimee project`, defined here in P1 — in which case those govern. An absent
project attributes to the team only; an **unknown, cross-team, or unauthorized
project is rejected**, never silently accepted or defaulted. P3/P4 cover the
cross-team and unauthorized-project cases in tests.

## §3 OIDC on the kb data plane — additive and optional

Promote the existing verifier to a first-class, documented data-plane
authenticator: when configured, a request bearing a valid OIDC JWT is
authenticated as the issuer-scoped identity `(iss, sub)` (never a bare `user:<sub>`) and resolved via §2.

**OIDC is never required — it is enabled only when the org configures it on kb**
(the verifier already registers *additively* after the owner token, opt-in via
`AIMEE_KB_OIDC_JWKS_FILE`). When OIDC is not configured:
- `cert:CN` (enrollment) is the always-present identity, and §2 binds teams off it.
- Human callers use the existing owner-token/bearer auth; team binding can map a
  configured owner/bearer to a default team.
- Nothing in §1–§4 (or in P2–P5) depends on OIDC being present. OIDC only *adds*
  a human-SSO identity source. A single-org box with no IdP runs entirely on
  certs.

**Revocation caveat.** OIDC JWTs are bearer tokens valid until their `exp`; the
verifier is stateless (signature + `exp`, no introspection or revocation lookup —
`auth_oidc.c`), so IdP-side revocation of a leaked token takes effect only at
expiry. A deployment that needs fast human-credential revocation must set short IdP
access-token lifetimes (≤15 min recommended); the machine spine stays real-time
via `cert.revoke`, enforced per request (not only at the mTLS handshake — see §2).
kb additionally enforces a **hard server-side ceiling on accepted token age** —
`now − iat ≤ configured max` (default 15 min) — rejecting an over-age token with a
typed error **regardless of the token's own `exp`**, so a leaked or misconfigured
long-lived token cannot outlive the ceiling even if the IdP sets a longer expiry. The
check is not bypassable by a bad `iat`: kb **requires a numeric `iat`, rejects a missing
or malformed one, rejects an `iat` in the future beyond a small clock-skew allowance,
and computes age with overflow-safe arithmetic** — critically, it **rejects `now < iat`
(beyond skew) *before* subtracting**, so the `time_t` subtraction never underflows into a
huge unsigned age that would pass the ceiling (tested). And
because authorization (team membership, entitlement) is re-resolved **per request from
the primary** (§2), a membership/entitlement change takes effect on the very next
request — the per-request rule applies to the OIDC/JWT path, not only to `cert.revoke`.

No SAML.

## §4 CLI + route surface

- `aimee team {create,list,show,add-member,remove-member}` → new `/v1/team/*`
  on aimee-kb, added to OpenAPI and `v1-method-coverage` (first-class,
  conformance-tested), gated behind an org-admin capability. The **org-admin
  capability has a defined source of authority**: it is held by a canonical
  issuer-scoped OIDC identity, a `cert:CN` identity, or a separately-identified owner
  principal; grants are stored RLS-constrained, checked from composite identity **on the
  primary per request**, and revocable — with an explicit bootstrap (the initial owner
  principal at install), so the capability is never implicit or self-asserted. **First-team
bootstrap** resolves the chicken-and-egg (an admin must create the first team before any
team context exists): first-team creation is an **owner-principal-only path** under a
dedicated bootstrap policy — the initial owner (from install) is authorized without a
pre-existing team, and all *subsequent* team/admin operations run under the normal
RLS-constrained runtime role.
- `kb-console`: extend the deny-by-default allowlist (`kb-console/acl.go` and
  `src/kb/http/kb_route_acl.c`) to expose team management in the existing
  OIDC-authenticated console.

## Acceptance criteria

- A team/project can be created and a member added, over CLI and `/v1`.
- An OIDC-authenticated request resolves to its `(iss, sub)` identity and team set; an
  mTLS `cert:CN` request resolves to its team.
- `kb_identity_resolve` returns stable results for OIDC, cert, and owner-token
  callers; an unknown identity returns an empty team set (deny), not a default
  admin fallback.
- OpenAPI and coverage are green for the new routes; org-admin gating is
  enforced server-side, not only in the console.
- kb↔Postgres uses `verify-full` TLS under a least-privilege runtime role (no DDL/
  superuser); tenant tables enforce team-scoped access (predicate/RLS) such that a
  cross-team read is denied at the **DB layer**, not only in the app — and does not
  leak between pooled requests on the shared role (per-request `SET LOCAL` context,
  reset on return).
- An encrypted backup restores and remains queryable (tested restore path). A
  revoked membership or certificate is honored **immediately** — the authorization
  read hits the primary, not a lagging replica.
- An OIDC request whose token age (`now − iat`) exceeds kb's configured ceiling is
  rejected with a typed error even if the token's own `exp` is further out; a
  membership/entitlement change is honored on the next request (authorization
  re-resolved from the primary, not cached for the connection's life).

## Testing

Unit also covers the highest-risk claims: composite-identity resolution (transport∩actor,
conflicting-identity reject), pooled-connection RLS isolation (no context leak across
requests on one connection), concurrent default-team creation (partial-unique holds),
project authorization (restricted/team-open, cross-team reject), per-request revocation on
a keep-alive connection, and `iat` edge cases (missing/malformed/future). Unit: JWT→team resolution (valid, expired, wrong `aud`, missing claim), cert→team,
membership CRUD, deny-on-unknown. Integration: stand up kb with a mock JWKS and
drive `/v1/team/*` plus an authenticated data-plane call end-to-end.

## Non-goals

No virtual keys. No user table (identities are external). No enforcement yet
(budgets are P4). No aimee-server changes — P1 is entirely kb-side.
