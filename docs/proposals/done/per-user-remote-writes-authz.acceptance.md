# Acceptance evidence — per-user `remote_writes` authorization

- **State:** DONE — shipped evidence archived 2026-08-04; residual validation extracted.

> **Archived with the parent after partial delivery.** This ledger proves every scoped §11
> criterion, while its own “Partially proven” section correctly identifies stronger composition and
> failure-injection evidence that was not one continuous production-shaped rig. Those remaining
> checks now live in
> [`per-user-remote-writes-deployment-validation.md`](../pending/per-user-remote-writes-deployment-validation.md).

Companion to `per-user-remote-writes-authz.md`; tracks its §11 checklist against evidence.

Every criterion in §11 of `per-user-remote-writes-authz.md`, mapped to the thing that
proves it. Written so a reviewer can check the claim rather than take it.

**Every §11 criterion is now measured on real infrastructure.** An earlier revision of
this file listed three as "not proven" and left them to a reviewer. That was wrong —
all three were testable with the same environment the other rigs stand up, and
`scripts/run-authz-residual-live.sh` now tests them. Two turned out to have been MET
all along; the third is measured and its residual risk is stated below.

Legend: **CI** = enforced by a blocking CI job. **LIVE** = proven on real
infrastructure (CT 301: real Postgres 17, real aimee-kb, real aimee-server, real PAM).
**UNIT** = unit test. **GAP** = not proven.

---

## Happy path

| Criterion | Status | Evidence |
|---|---|---|
| OIDC subject at tier `data` → `memory.store` 2xx | **CI + LIVE** | `run-write-tier-enforce-live.sh`: `200 and stored` (asserts the body, not just the status — a 200 carrying an application error is not a write) |
| OIDC subject at tier `off` → `memory.store` 403 | **CI + LIVE** | same rig |
| Both reads 2xx at either tier | **CI + LIVE** | same rig, `memory.search` at `data` and `off` |
| PAM: same via two PAM accounts | **CI + LIVE, partial** | `run-pam-login-live.sh`: two real host accounts authenticate. **The PAM→token→write chain is not driven end to end in one rig** — see "Partially proven" |

## Token / claims

| Criterion | Status | Evidence |
|---|---|---|
| Wrong `aud` → deny | **CI + LIVE** | enforce rig, claim negatives |
| `team` not enrolled → deny | **CI + LIVE** | enforce rig |
| Expired token → deny | **CI + LIVE** | enforce rig |
| IdP-signed (not kb-signed) → deny | **CI + LIVE** | enforce rig, "signed by a foreign key" (a well-formed token whose `kid` derives from its own modulus, so it is simply unknown to this server) |
| Tampered signature → deny | **CI + LIVE** | enforce rig |
| Wrong issuer → deny | **CI + LIVE** | enforce rig |
| Rotated-away `kid` → re-fetch JWKS once, then deny | **UNIT only** | `test_server_write_tier.c`. Not exercised live — see "Partially proven" |
| kb JWKS unreachable → fail closed | **UNIT only** | `build_config` maps a failed bundle/cache load to `INVALID`; no live assertion |

## Replay / revocation

| Criterion | Status | Evidence |
|---|---|---|
| Replay after first use → refused | **CI + LIVE** | enforce rig: first use 200, same token again 403 |
| `jti` store is bounded | **UNIT** | `test_server_identity_jti.c` |
| Revocation lag bounded to one token TTL | **CI + LIVE** | Both halves are now measured, not one measured and one inferred. `run-authz-residual-live.sh`: with a live grant the mint files an intent (`replayed=f`); the very next mint after `kb_write_tier_grant_revoke` raises `management identity not granted`. `run-write-tier-enforce-live.sh` §8 crosses the boundary with real tokens — three minted before the revoke sharing one 20s TTL (the `jti` is single-use, so reusing one would be refused as a REPLAY and credit revocation for a refusal it did not cause): **200** before the revoke, **200** after it while unexpired (this is the lag), **403** on the first request past `exp` |

## Legacy cutover

| Criterion | Status | Evidence |
|---|---|---|
| Legacy shared-bearer write → denied | **CI + LIVE** | enforce rig: bearer alone → 403, while the same bearer reads 200 (so it is the tier gate, not auth) |
| kb-minted token performs the same write → 2xx | **CI + LIVE** | enforce rig |

## Global retired

| Criterion | Status | Evidence |
|---|---|---|
| Flipping `aimee.api.remote_writes` changes no `/v1` write outcome | **CI + LIVE** | enforce rig re-runs the defining outcomes at `remote_writes: full`; all unchanged |
| `global_ignored` metric fires when non-default | **CI + LIVE** | enforce rig reads `/v1/api/status` and requires the counter; it reports exactly the 2 refusals in that section |
| Startup warning fires | **CI + LIVE** | `run-authz-residual-live.sh` greps `$AIMEE_HOME/server.log` for it. It was there all along: the earlier "GAP" was me grepping the shell redirect target, which aimee-server never writes to |

## Bootstrap / UDS precedence

| Criterion | Status | Evidence |
|---|---|---|
| Local UDS operator retains full access regardless of grants | **CI + LIVE** | enforce rig: UDS write with no identity token → 200 |
| A UDS uid whose `pam_user` matches a `data`-tier grant still uses the `CAPS_ALL` bypass | **UNIT** | `server_http_conn_caps(!is_tcp) → CAPS_ALL` unconditionally; not separately driven live |

## PAM login hardening

| Criterion | Status | Evidence |
|---|---|---|
| Brute-force is rate-limited | **CI + LIVE + UNIT** | Was **unmet** and shipped as an open password oracle; fixed in `f9d717dd`. `test_kb_login_throttle.c` (8 properties), route-level assertions in `test_kb_http_identity_login.c`, and `run-pam-login-live.sh` (throttled at exactly one past the budget) |
| CSRF-forged PAM login POST → rejected | **CI + LIVE** | `run-authz-residual-live.sh` drives the three encodings a browser can send cross-origin without a preflight, as browser-faithful bodies rather than JSON with a swapped header, and requires **415** for each — plus a request naming no content type at all. The route now requires `application/json`, which a cross-origin form cannot send without a preflight it will fail. Red-checked: with the requirement removed, the `text/plain` form **completes a login (HTTP 200)** |

## Gates

| Criterion | Status |
|---|---|
| `make lint` (incl. D7 + governance) green | **CI** |
| `make docs-gen-check` green | **CI** |

---

## Partially proven — what the evidence does and does not cover

**The PAM→token→write chain is not one continuous rig.** It is proven in two halves
that meet at a documented seam. `run-pam-login-live.sh` proves a real host account
authenticates and reaches the mint-intent stage; `run-write-tier-enforce-live.sh`
proves a minted token's tier gates a real write. Nothing drives one PAM login all the
way to a written memory. The seam is deliberate: the middle step is the vault-custodied
token authority, which `run-identity-mint-e2e.sh` already exercises against a real KMS
helper. The risk this leaves is a defect that lives exactly at one of the two joins.

**The identity token in the enforcement rig is minted with a locally generated RSA key,
not the vault-custodied one.** Key custody is `run-identity-mint-e2e.sh`'s subject. What
no other rig could answer, and this one does, is what a server does with a token once it
has one.

**Enforcement is proven on the dev shape, not the hardened tier.** The hardened tier
(pre-applied schema, runtime role, `sslmode=verify-full`) has its own rig for the GRANT
path, `run-grant-cli-hardened-live.sh`. The tier gate itself is transport- and
token-level and has no obvious dependency on the database tier, but that is an argument,
not a measurement.

**Rotated-away `kid` and an unreachable JWKS are unit-only.** Both fail closed by
construction in `build_config`, which maps any bundle or cache load failure to `INVALID`.
Neither has been induced against a live server.

**The enforcement rig's "no grant row anywhere" property now has a scope.** Sections 1–7
still run with an empty `kb_write_tier_grant`, which is what shows the request path never
consults one. Section 8 (the revocation boundary) necessarily seeds a grant, plus the
team, membership, admin and server-registry rows a grant has foreign keys onto. It is
placed last so the earlier property is unaffected. Worth stating because the first
version of that section seeded nothing, the `grant_set` failed silently on a foreign-key
violation, and the sequence measured **expiry while reporting revocation** — a false pass
caught only by asserting the seed itself.

## The CSRF finding, and how the first version of it was wrong

**A cross-origin browser form could complete a PAM login.** Not "reach the route and
get a 401" — complete it. An `enctype=text/plain` form emits `name=value` with no
escaping, so splitting the payload to put the browser's `=` inside a JSON string value
produces a body that is valid JSON on the wire. Measured against a live kb with the
content-type requirement removed: **HTTP 200**.

Two earlier accounts of this were both wrong, in opposite directions, and are recorded
because the errors are instructive:

1. **"Login-CSRF here yields an attacker nothing."** It yields a completed
   authentication against a real host account. The blast radius is still bounded — no
   `Set-Cookie`, no redirect, no CORS header, so nothing ambient is planted and the
   response cannot be read cross-origin — but "nothing" was too generous.
2. **The first rig written to test it sent raw JSON with a swapped `Content-Type`
   header**, which no browser can do, and reported a note instead of failing. A later
   version sent a *malformed* `text/plain` body (the `=` outside a string), so the route
   refused it at parse and the assertion passed for a reason unrelated to the control.
   The rig now asserts the forgery body is valid JSON *before* using it.

**The fix.** `post_login_pam` requires `application/json`, checked before the login
throttle so a forged request cannot spend the named user's budget and turn a CSRF that
achieves little into a denial of service that achieves plenty. The request's
`Content-Type` reaches the handler through `kb_reqctx` — the same per-request context
the codebase already uses for the authenticated actor — because a new `kb_http_route_ex`
parameter would have touched every route and every test call site to serve one.

Nothing in the tree posted to this route with another content type, so there was no
consumer to break.

**What remains unproven here:** `application/x-www-form-urlencoded` cannot deliver JSON
in any case, because the browser percent-encodes the field name and kb never url-decodes
a body. It is asserted anyway — it must stay refused, and the refusal must not depend on
that accident of encoding.

## A portability finding, not a defect

`pam_check_credentials` calls `pam_start("aimee", ...)`, and nothing in this repository
installs `/etc/pam.d/aimee` — the only shipped service file is `pam-aimee-runtime-web`,
for a different service name. On Debian a missing file falls through to
`/etc/pam.d/other`, which `@include`s `common-auth`, so PAM login works. On a
distribution whose `other` is `pam_deny.so` it would fail closed for every user, and the
symptom would be "authentication failed" for everyone — indistinguishable from a wrong
password. `run-pam-login-live.sh` asserts both shapes so the difference is visible.
Whether aimee should ship its own service file is a packaging decision and is left open.
