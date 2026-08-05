# Proposal: Per-user `remote_writes` authorization

- **State:** DONE — shipped scope archived 2026-08-04; residual validation extracted.

> **Archived after partial delivery.** Per-user OIDC/PAM login, tiered grants, signed token minting,
> enforcement, operator grant administration, and blocking live rigs shipped in `bcf7641d7` and its
> hardening follow-ups. The remaining composed deployment/custody/failure-injection proof is now
> [`per-user-remote-writes-deployment-validation.md`](../pending/per-user-remote-writes-deployment-validation.md).

Implemented 2026-07-28. The first-administrator bootstrap supplement is archived in
  [wizard-first-user-bootstrap](wizard-first-user-bootstrap.md).
- **Charter roles:** Enforce / Constrain-Verify / Gate-Promote.
- **Depends on (DONE, merged):** the P5 JWKS + token-authority substrate — **P5-C2b** signed JWKS
  publication (`src/kb/kb_mgmt_jwks_publication.c`), **P5-C2c** authenticated server→kb JWKS fetch +
  durable cache (`src/server/server_mgmt_jwks_cache.c`; wired at `src/server/server.c:2201` via
  `kb_client_mtls_management_jwks_fetch`), and **P5-C2d** the kb token authority
  (`kb_mgmt_token_authority_sign_pkcs8`, `src/kb/kb_mgmt_token_authority.c:245`). **Also depends on**
  P1 (identity / teams / RLS).
- **Does NOT depend on** the *deferred* part of P5 — the kb→server management channel / server registry
  (P5 §1–§2). The kb-signed identity token here is carried to the server **by the user** (browser /
  thin client) on `/v1`, not pushed by kb, so no kb→server dial is required.
- **Thesis:** The `/v1` write gate is already parameterized on a `remote_writes` tier; today that tier
  is one process-global value applied to every TCP caller behind a single shared bearer. Make the tier
  a function of the **authenticated individual user**. aimee does **not** build its own login or
  identity store — it reuses standard identity: **OIDC when enabled, otherwise the host's PAM stack via
  aimee-kb**. Authentication of interactive per-user write flows always terminates at **aimee-kb**;
  aimee-server only **enforces** the tier kb authenticated, verified from a kb-signed token over the
  already-merged server→kb JWKS path. Reuse the existing gate, the token authority, the JWKS
  publish/fetch, and the per-(server, team) management config projection. Add no new policy object, no
  bespoke password store, no new audit family.

## 1. Problem

`remote_writes` (`off` | `data` | `full`) is a single global server setting enforced in
`server_http_route_allowed_caps` (`src/server/server_http.c`): data-plane writes (`g_v1_write_ops`)
open at `data`, privileged/exec routes need `full`, all behind **one shared bearer** (plus `scope:`
bearers, read-only). Two callers with the same bearer are indistinguishable, so a deployment cannot
grant "user A may write, user B may only read" without a second server. There is no per-user login into
the `/v1` data plane today.

## 2. Principle & the scope of "kb is the authenticator"

- **aimee-kb authenticates interactive per-user write flows.** aimee-server never verifies OIDC and
  never handles passwords for those flows.
- **Two mutually-exclusive modes per kb:** **OIDC** (when an issuer is configured) *or* **local PAM**.
- **aimee owns only the grant** — `{subject → team, remote_writes tier}`. Credentials belong to the IdP
  (OIDC) or the host/enterprise PAM stack.
- **Explicitly carved-out non-interactive authenticators (F1):** the "kb is sole authenticator" clause
  is scoped to *interactive per-user write authorization*. It does **not** cover (a) the **local UDS
  operator** — `is_tcp==0 → CAPS_ALL`, OS-attested `peercred`, which continues to bypass per-user tiers
  entirely (§7); (b) the **one-time bootstrap bearer** (`handle_api_rotate_bearer`, rotate-only,
  single-use-in-effect, audited) and **single-use enrollment tokens**, which bootstrap the *first
  admin* who then configures kb. These are aimee-owned bootstrap credentials by design; they establish
  administrative identity and keep their existing lifecycle guarantees (rotate-only / single-use /
  audited). The setup wizard's enrollment bearer is a further explicit carve-out: the bearer itself
  never grants writes, but signing its client-held CSR binds a local first-owner `full` grant to that
  exact mTLS certificate. Additional interactive users still use the KB-authenticated token path.
  The carve-outs are stated, not hidden.

## 3. Login & identity flow

Entry is a **new** aimee-server web-GUI login surface built on existing primitives — the TOFU rotate/
enrollment handlers (`handle_api_rotate_bearer`, `server_api_status.c:195`) and the server's existing
browser authorization-code redirect machinery (`git_oauth_github_web_start` → `{authorize_url,
redirect_uri}`, `oauth_pkce.c`). (The wizard UI is new; it does not reuse a pre-existing
`aimee-thinclient-adoption` symbol — that name was a mislabel.) The user provides the aimee-kb endpoint;
**kb declares its auth mode**, and the flow forks:

- **OIDC** — server-initiated redirect **delegates the auth-code exchange to kb** (kb is the relying
  party: it holds the issuer profile, the client secret in vault, and the issuer JWKS — grounded by the
  `AIMEE_KB_OIDC_AUDIENCE`/JWKS configuration and the merged token authority; the auth-code exchange
  runs at kb's web/OIDC tier). kb authenticates, resolves `{subject, team, tier}`, and mints the
  kb-signed token (§4) via the merged token authority.
- **Local PAM** — the wizard shows a login form; kb verifies `username`+secret against the host PAM
  stack and mints the same token. Concrete, testable PAM shape:
  - **Mediator:** `pam_authenticate` through a **dedicated `/etc/pam.d/aimee-kb` service** invoked via a
    **`pam_exec`/helper** path; kb does **not** read `/etc/shadow` itself and does **not** run as root.
    kb runs as a **dedicated non-root system account created by the kb package** (per the P5 hardening
    pattern, PR #1729 / #1808); the package ships `/etc/pam.d/aimee-kb` permitting that account to
    invoke the service. Where `pam_unix` needs shadow, the standard setuid `unix_chkpwd` helper performs
    the check — kb never holds shadow read or root itself.
  - **Service file:** shipped by the kb package at `/etc/pam.d/aimee-kb`, `root:root 0644`.
  - **No local hash handling:** kb never caches, compares, or persists password hashes; the credential
    lives only for the duration of the PAM call.
  - **Credential transport:** the wizard POSTs `username`+secret to a **loopback/HTTPS**, **rate-limited**,
    **CSRF-protected** kb endpoint; the secret is `explicit_bzero`'d after the PAM call and **never
    logged**.

Either way, aimee-server verifies the token over the **already-merged** server→kb JWKS path
(`server_mgmt_jwks_cache`, HTTPS pinned to kb's CA — §Depends), reads the claims, and feeds the tier
into the `/v1` write gate. The server never talks to the IdP and never sees a password.

## 4. kb-signed token contract (F4)

The token minted by `kb_mgmt_token_authority_sign_pkcs8` carries a **pinned claim set**:
`iss=kb`, `aud=<this server_id>`, `sub`, **`team`**, `tier ∈ {off,data,full}`, `jti`, `iat`, short
`exp`. The server verifies, **per request**: signature against the cached kb JWKS (by `kid`); `iss==kb`;
`aud==this server_id`; **`team` ∈ this server's enrolled teams**; `exp` not passed; and `jti` not seen
(§9 replay). A subject granted at team X therefore cannot be replayed against team Y — the grant lookup
(§6) and the enforced `team` claim must agree. Any failed check → **deny all writes** (fail closed).

## 5. Enforcement seam (small, contained)

`server_http_effective_conn_caps()` (def. ~L319) / `server_http_route_allowed_caps()` (def. ~L358)
already take `remote_writes` as a parameter. At the request-handling seam in `src/server/server_http.c`
the code passes the process-global `g_remote_writes` into those functions (the exact call-site line is
re-pinned during implementation planning — nit). The behavior change is to pass the **per-request tier**
from the verified token instead. The gate's decision logic — which ops are data-writes, which need `full` — does
not change.

## 6. Tier storage, administration & migration

Extend the per-(server, team) management config (`server_mgmt_read_project_config` /
`read_config_projection_valid`, backed by the db2 management schema) to carry **subject-keyed** grants
`{subject → tier}` within a `(server, team)`. **Migration:** existing `(server, team)` rows have **no
`subjects` map**; the new path treats "no grant" as **deny** (already fail-closed), so no appliance
silently *widens*. Operators populate grants post-upgrade (documented procedure); we do **not** auto-map
the old global into a wildcard grant (that would re-introduce a global authorizer — see §8). This is the
same fail-closed posture as F2, applied at the data layer.

## 7. Bootstrap & root of trust; UDS precedence

The irreducible root of trust needs no default credential: the **local UDS operator** —
`server_http_conn_caps` returns `CAPS_ALL` for `is_tcp==0` (`server_auth.c` builds `uid:<N>` from
`peer_uid` via `ATTEST_UDS_PEERCRED`) — **bypasses `remote_writes` entirely** and is un-lockout-able.
**Precedence is explicit and testable:** for `ATTEST_UDS_PEERCRED` the `is_tcp==0` path returns
`CAPS_ALL` unchanged and the per-user tier is **not** consulted, even if that uid's name matches a
PAM/OIDC grant; for all TCP transports the per-user tier from the verified token governs. That operator
configures kb (OIDC issuer profile *or* PAM + the `{subject → team, tier}` grants). A first *remote*
admin bootstraps via the one-time bootstrap bearer / enrollment token (§2 carve-out).

## 8. Legacy-caller cutover (F2 — hard cutover + migration guide)

Per decision: on rollout the legacy **standing** shared bearer / `AIMEE_API_BEARER` path (the rotated
per-deployment bearer, which does authorize reads and writes today) **stops authorizing writes**
(fail-closed); the reads it permits today are unaffected. Note this is distinct from the *one-time
bootstrap* bearer, which has always been **rotate-only** (`handle_api_rotate_bearer` refuses every other
TCP route until rotated), so no read path ever relied on it and it is untouched here. The deliverable
includes a **migration guide**
enumerating every affected `/v1` caller and its replacement:
- **Interactive** (thin clients, webchat) → obtain a kb-issued token via the wizard login (§3).
- **Non-interactive** (audit, cron, service integrations) → **kb-minted service-account tokens**
  (same token authority + claim set, longer `exp`, a service `sub`, a fixed tier).
- The now-non-authorizing global `aimee.api.remote_writes` emits a **startup warning** and increments a
  **`remote_writes.global_ignored`** metric while it remains parsed, so an on-call operator is never
  misled into thinking it still gates (suggestion). Flipping it changes no `/v1` write outcome
  (acceptance §12).

## 9. Security considerations

- **Fail closed** on every unresolved identity / failed claim check; never widen on ambiguity.
- **mTLS transport unchanged** — it establishes the connection; it is not the authorization identity.
- **Replay:** short `exp` + server-local **bounded `jti` cache** (LRU/TTL sized to the token TTL); a
  replayed token after first use is rejected. **Revocation lag:** revoking a subject's grant takes
  effect at the next token `exp`; a key-level revocation takes effect once the kb JWKS
  signed-generation advances. The lag is therefore bounded to **one token TTL or one JWKS-generation
  advance, whichever is shorter**, and is tested.
- **PAM least-privilege** as pinned in §3 — no kb shadow-read, no root, no credential persistence,
  `explicit_bzero`, never logged.
- **No new identity store** — the IdP / OS-PAM stays authoritative, so disabling a user there disables
  aimee access.
- **Audit** — write decisions remain on the existing governed-action audit bus; no new vocabulary.

## 10. Phased implementation (one PR per slice, off `testing`, CI-green, never pushed to `testing`)

1. This proposal (review gate — re-review after this revision).
2. **kb authentication + token minting** — auth-mode declaration; OIDC relying-party delegation *and*
   PAM local-account auth (dedicated `aimee-kb` PAM service + helper, §3); mint the §4 token via the
   merged token authority. Unit tests.
3. **Per-(server, team, subject) grant storage + admin surface** — extend the projection + db2 schema;
   grant set/get; empty-map migration (§6). Tests.
4. **Server: wizard login + token verification + gate rewire + legacy cutover** — redirect (OIDC) /
   login form (PAM); verify the §4 claim set via server→kb JWKS; feed the tier into the gate; retire the
   global authorizer with warning+metric; hard-cutover the legacy bearer (§8). Tests.
5. **e2e + governance + docs** — config-mode matrix asserts per-user tiers on **both** OIDC and PAM
   paths **and** the unhappy paths in §12. **PAM e2e fixture:** the local-stack container must ship a
   working `pam_unix`-backed `/etc/pam.d/aimee-kb` and two known test users (flagged to the implementer;
   `scripts/aimee-local-stack-e2e.sh` already runs in a fixed-root env, so this is feasible). Migration
   guide; `make lint` (incl. D7) + `make docs-gen`.

## 11. Acceptance criteria (closed checklist)

**Happy path.** OIDC: subjects with tiers `data`/`off` → `memory.store` `2xx`/`403`, both reads `2xx`.
PAM: same via two PAM accounts.
**Token/claims.** Token with wrong `aud` → deny; `team` not enrolled on this server → deny; expired
token → `401`; token signed by a rotated-away `kid` → server re-fetches JWKS once then denies; an
**IdP-signed (not kb-signed) token** → deny; kb JWKS endpoint unreachable → **fail closed** (deny).
**Replay/revocation.** Replay after first successful use → `401`; `jti` store is bounded (tested); after
kb revokes a subject's grant, the next call past the documented lag → deny.
**Legacy cutover.** A legacy shared-bearer / `AIMEE_API_BEARER` write after rollout → denied; a
kb-minted service-account token performs the same write → `2xx`.
**Global retired.** Flipping `aimee.api.remote_writes` changes **no** `/v1` write outcome; the startup
warning + `global_ignored` metric fire when it is non-default.
**Bootstrap.** The local UDS operator retains full access regardless of grants; a UDS uid whose
`pam_user` matches a `data`-tier grant still uses the `CAPS_ALL` bypass (precedence §7).
**PAM login hardening.** CSRF-forged PAM login POST → rejected; brute-force is rate-limited.
`make lint` (incl. D7 + governance) and `make docs-gen-check` stay green.

## 12. Out of scope

Read-tier partitioning, per-route custom grants beyond `off`/`data`/`full`, changes to mTLS transport
policy, a self-service user directory or password lifecycle (owned by the IdP / OS-PAM), the deferred
P5 kb→server management channel / server registry, and the KB's own internal tenancy enforcement. These
remain with their current owners.
