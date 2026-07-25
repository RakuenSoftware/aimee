# Proposal: Per-user `remote_writes` authorization

- **State:** DRAFT — 2026-07-25; awaiting roundtable review. No implementation has started.
- **Charter roles:** Enforce / Constrain-Verify / Gate-Promote.
- **Depends on:** **P5** (OIDC control plane — issuer profiles, the kb-signed identity token, and JWKS
  fetched over the server→kb channel) and **P1** (identity / teams / RLS). Shares the OIDC verifier and
  the kb-signed-token mechanism with P5 rather than inventing new ones.
- **Thesis:** The `/v1` write gate is already parameterized on a `remote_writes` tier; today that tier is
  one process-global value applied to every TCP caller behind a single shared bearer. Make the tier a
  function of the **authenticated individual user**. aimee does **not** build its own login or identity
  store — it reuses standard identity: **OIDC when enabled, otherwise the host's PAM stack via
  aimee-kb**. Authentication always terminates at **aimee-kb**; aimee-server only **enforces** the tier
  kb authenticated. Reuse the existing gate, the existing adoption/enrollment flow, the existing
  browser-redirect machinery, the OIDC verifier + JWKS fleet, and the per-(server, team) management
  config projection. Add no new policy object, no bespoke password store, no new audit family.

## 1. Problem

`remote_writes` (`off` | `data` | `full`) is a single global server setting enforced in
`server_http_route_allowed_caps` (`src/server/server_http.c`): data-plane writes (`g_v1_write_ops`)
open at `data`, privileged/exec routes need `full`, all behind **one shared bearer** (plus `scope:`
bearers, read-only). Two callers with the same bearer are indistinguishable, so a deployment cannot
grant "user A may write, user B may only read" without a second server. The tier is a per-appliance
switch, not a per-principal grant — and there is no per-user login into the `/v1` data plane at all
today (the only OIDC SSO is kb-console, operator-facing).

## 2. Principle: reuse standard identity, never reimplement it

- **aimee-kb is the sole authenticator.** aimee-server never verifies OIDC and never handles passwords.
- **Two mutually-exclusive modes per kb:** **OIDC** (when an issuer is configured) *or* **local PAM**.
- **aimee owns only the grant** — the `{subject → team, remote_writes tier}` mapping. Credentials are
  owned by the IdP (OIDC) or the host/enterprise PAM stack. No aimee password hashing, rotation,
  lockout, or user directory.

## 3. Login & identity flow

Entry is the aimee-server **web-GUI adoption wizard** (the existing TOFU adoption flow,
`aimee-thinclient-adoption`). The user provides the aimee-kb endpoint; **kb declares its auth mode**,
and the flow forks:

- **OIDC** — server-initiated redirect **delegates the auth-code exchange to kb** (kb is the relying
  party: it owns the issuer profile, client secret in vault, and JWKS — P5 §0.1; `kb-console` already
  performs this exchange). The server reuses its existing browser-redirect machinery
  (`git_oauth_github_web_start` → `{authorize_url, redirect_uri}`, `oauth_pkce.c`). kb authenticates,
  resolves `{subject, team, tier}`, and mints a short-lived **kb-signed token** (P5 §3: `iss=kb`,
  `aud=this server`, `sub`, tier, `jti`, short expiry).
- **Local PAM** — the wizard shows a **login form**; kb verifies `username`+secret via
  `pam_authenticate` against an `aimee-kb` PAM service (the host's battle-tested stack: `pam_unix`,
  LDAP-via-PAM, etc.). On success the subject is the PAM username; kb resolves `{subject, team, tier}`
  and mints the **same** kb-signed token.

Either way, aimee-server verifies kb's token signature by fetching kb's JWKS over the **server→kb
channel it already trusts** (its own `clientAuth` cert, HTTPS pinned to kb's CA — P5 §3 routes JWKS
this way, never over the kb→server mgmt channel, to avoid circularity), reads `sub` + tier, and feeds
the tier into the `/v1` write gate. The server never talks to the IdP and never sees a password.

## 4. The enforcement seam (small, contained)

`server_http_effective_conn_caps()` / `server_http_route_allowed_caps()` already take `remote_writes`
as a parameter. At the request seam (`src/server/server_http.c`, ~L1687) the code passes the
process-global `g_remote_writes`. The whole behavior change on the server is to pass the **per-request,
per-user** tier (from the verified kb-signed token) there instead. The gate's decision logic — which
ops are data-writes, which need `full` — does not change.

## 5. Tier storage & administration

Extend the existing per-(server, team) management config (`server_mgmt_read_project_config` /
`read_config_projection_valid`, backed by the db2 management schema) to carry **subject-keyed**
`remote_writes` grants within a `(server, team)`. A grant is `{subject → tier}`; administered through
the existing management API — no new policy surface. kb resolves `(server_id, team, subject) → tier`
when it mints the token.

## 6. Bootstrap & root of trust (the first admin)

The irreducible root of trust already exists and needs no default credential: the **local UDS
operator** — `server_http_conn_caps` returns `CAPS_ALL` for `is_tcp==0` ("same-user,
filesystem-attested"; `server_auth.c` builds a `uid:<N>` principal from `peer_uid` via
`ATTEST_UDS_PEERCRED`) — and it **bypasses `remote_writes` entirely**, so the local operator is
un-lockout-able. That operator configures kb: enables the OIDC issuer profile, **or** relies on host
PAM and sets the `{subject → team, tier}` grants. Credentials themselves are owned by the IdP / OS-PAM.
A first *remote* admin (no local shell) bootstraps via the existing one-time bootstrap bearer
(rotate-only) or a single-use enrollment token.

## 7. Enforcement & the global setting

- The verified per-user tier is passed into the gate in place of `g_remote_writes`.
- **Per-user fully replaces the global.** `aimee.api.remote_writes` no longer authorizes requests
  (retained initially as parsed-but-non-authorizing for telemetry/back-compat; deletion is a later
  cleanup). `server_mgmt_read_source.c` and the startup status line are updated.
- **Fail closed:** an unmatched/anonymous identity resolves to `off` (writes denied).

## 8. Phased implementation (one PR per slice, off `testing`, CI-green, never pushed to `testing`)

1. This proposal (review gate).
2. **kb authentication + kb-signed session token** — auth-mode declaration; OIDC relying-party
   delegation *and* PAM local-account auth (`libpam`, `aimee-kb` PAM service, least-privilege helper);
   mint the short-lived kb-signed token for both modes. Unit tests.
3. **Per-(server, team, subject) tier storage + admin grant surface** — extend the mgmt config
   projection + db2 schema; grant set/get; migration. Tests.
4. **Server: wizard login entry + verify token + gate rewire** — redirect (OIDC) / login form (PAM)
   entry; verify the kb-signed token via server→kb JWKS; feed the tier into the gate; retire the global
   as authz source; fail-closed default. Tests.
5. **e2e + governance + docs** — the local-stack config-mode matrix asserts per-user tiers on **both**
   the OIDC and PAM paths (user A=`data`→`store` 2xx, user B=`off`→`store` 403; both reads 2xx;
   unmatched→denied); docs; `make lint` (incl. D7) + `make docs-gen`.

## 9. Security considerations

- **Fail closed** on every unresolved identity; never widen on ambiguity.
- **mTLS transport unchanged** — it establishes the connection; it is not the authorization identity.
- **kb-signed token** is short-lived and carries a `jti` for server-local replay rejection (P5 §3);
  the server verifies `iss`/`aud`/`exp`/signature and pins the JWKS to kb's CA.
- **PAM least-privilege** — aimee-kb authenticates via a helper (e.g. `unix_chkpwd`) or a dedicated PAM
  service; it does not run as root to read shadow, and it never stores or logs credential material.
- **No new identity store** — no aimee-owned passwords; the IdP / OS-PAM remains authoritative, so
  disabling a user there disables aimee access.
- **Audit** — write decisions remain observable through the existing governed-action audit bus; no new
  audit vocabulary.

## 10. Acceptance criteria

- OIDC mode: two subjects with tiers `data` and `off` produce `memory.store` → `2xx` and `403`; both
  reads succeed. PAM mode: the same via two PAM accounts.
- An unmatched credential is denied all writes (fail closed); `aimee.api.remote_writes` no longer
  affects the decision.
- The local UDS operator retains full access regardless of grants (bootstrap is never locked out).
- `make lint` (incl. D7 + governance) and `make docs-gen-check` stay green.

## 11. Out of scope

Read-tier partitioning, per-route custom grants beyond `off`/`data`/`full`, changes to mTLS transport
policy, a self-service user directory or password lifecycle (owned by the IdP / OS-PAM), and the KB's
own internal tenancy enforcement. These remain with their current owners.
