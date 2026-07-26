# Acceptance evidence — per-user `remote_writes` authorization

- **State:** IN PROGRESS — 2026-07-26. Companion to `per-user-remote-writes-authz.md`;
  tracks its §11 checklist against evidence. Moves to `done/` with the proposal.

Every criterion in §11 of `per-user-remote-writes-authz.md`, mapped to the thing that
proves it. Written so a reviewer can check the claim rather than take it.

**Read the "Not proven" section first.** Three criteria are not met, and one of them is
a deliberate scope call that a reviewer may want to overrule.

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
| Revocation lag bounded to one token TTL | **GAP** | not tested — see "Not proven" |

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
| Startup warning fires | **GAP** | the metric is asserted; the startup log line is not |

## Bootstrap / UDS precedence

| Criterion | Status | Evidence |
|---|---|---|
| Local UDS operator retains full access regardless of grants | **CI + LIVE** | enforce rig: UDS write with no identity token → 200 |
| A UDS uid whose `pam_user` matches a `data`-tier grant still uses the `CAPS_ALL` bypass | **UNIT** | `server_http_conn_caps(!is_tcp) → CAPS_ALL` unconditionally; not separately driven live |

## PAM login hardening

| Criterion | Status | Evidence |
|---|---|---|
| Brute-force is rate-limited | **CI + LIVE + UNIT** | Was **unmet** and shipped as an open password oracle; fixed in `f9d717dd`. `test_kb_login_throttle.c` (8 properties), route-level assertions in `test_kb_http_identity_login.c`, and `run-pam-login-live.sh` (throttled at exactly one past the budget) |
| CSRF-forged PAM login POST → rejected | **GAP, by argument** | see "Not proven" |

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

## Not proven

1. **Revocation lag.** §11 requires that after kb revokes a subject's grant, the next
   call past the documented lag is denied, and that the lag is bounded by one token TTL
   or one JWKS-generation advance. Nothing tests this. It needs a rig that revokes a
   grant and then advances time or the generation, which neither existing rig does.

2. **CSRF on the PAM login POST.** No CSRF token, and this is an argument rather than an
   omission: the route is a JSON API that sets no cookie and establishes no browser
   session, its response is not readable cross-origin, and an attacker must supply
   credentials they already know — so login-CSRF yields them nothing. kb does not enforce
   a request `Content-Type`, so a cross-origin form CAN reach the route; what it achieves
   is filing an intent for the attacker's own account from someone else's browser, with
   no readable response. **A reviewer who disagrees should say so** — the fix (requiring
   a JSON content type) is small, and the reason it is not here is judgement about
   impact, not effort.

3. **The startup warning for a non-default `aimee.api.remote_writes`.** The metric half
   is asserted live; the log line is not.

## A portability finding, not a defect

`pam_check_credentials` calls `pam_start("aimee", ...)`, and nothing in this repository
installs `/etc/pam.d/aimee` — the only shipped service file is `pam-aimee-runtime-web`,
for a different service name. On Debian a missing file falls through to
`/etc/pam.d/other`, which `@include`s `common-auth`, so PAM login works. On a
distribution whose `other` is `pam_deny.so` it would fail closed for every user, and the
symptom would be "authentication failed" for everyone — indistinguishable from a wrong
password. `run-pam-login-live.sh` asserts both shapes so the difference is visible.
Whether aimee should ship its own service file is a packaging decision and is left open.
