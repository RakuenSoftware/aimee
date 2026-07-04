# Proposal: aimee-kb web console (dashboard · accounts · governance)

- **State:** pending (roundtable-reviewed 2026-07-04, v2 — findings folded in)
- **Author:** JBailes
- **Drafted:** 2026-07-04

## Goal

Give **aimee-kb** — the standalone, shareable knowledge-base service that owns DB2 —
its own browser UI, built on **SmoothGUI** the same way `aimee-webchat` is. Today a
shared/company KB is administered only over the CLI and raw `/v1` HTTP. Operators of a
shared KB need a UI to:

1. **Dashboard** — see the KB's health and throughput at a glance: ingest/curator/worker
   queue depth, memory/vector/code-index counts, collection & release state, pipeline
   throughput, contradictions, calibration readiness.
2. **Accounts** — manage *who and what* can reach the KB: enroll clients (mint tokens /
   `aimee://` strings), list / revoke / renew issued client certs, configure BYO-OIDC
   (JWKS / iss / aud → scope), and browse the scope lattice.
3. **Governance** — browse and author **decision records** (`decision_log`, one-active-
   per-scope), review the **policy-verdict action audit** (`audit_events`), and work the
   curator **review queue**.

The console is a **separate service that fronts `aimee-kb`'s `/v1` directly**, so it works
in a KB-only / shared-KB deployment where **no `aimee-server` is colocated** — the whole
point of a company-wide KB. (Operator-locked 2026-07-04: *new dedicated console service*;
*build on the KB's existing auth substrate*, not a new user/RBAC model.)

## §0 What already exists (so we don't rebuild it)

**The GUI template — `aimee-webchat` (`webchat/*.go` + `frontend/`).** A standalone **Go
thin client** that serves the vendored-SmoothGUI React 19 + Vite SPA (`frontend/`, bundled
to a single inlined `dist/index.html`) and proxies `/api/*` to a backend `/v1`. It holds no
domain DB — only a SQLite **session** store — does PAM login, auto-generates TLS, and
imports `github.com/RakuenSoftware/smoothgui/{auth,…}`. `frontend/` is the SmoothGUI shell
(`App.tsx` router + nav) with per-page components. **We mirror this shape** — a new
`aimee-kb-console` Go service + a second SPA build reusing the vendored SmoothGUI tarball
and the same Vite tooling — but point the proxy at the **KB** `/v1` and swap PAM for the
KB's own auth. (⚠️ S0 acceptance criterion: verify SmoothGUI's `auth` package is
auth-method-agnostic; it was built for webchat's **PAM** flow. If it is PAM-shaped, factor
an adapter in the console rather than coupling the KB's auth surface to a PAM-typed upstream.)

**The auth / accounts substrate — already built (distributed-mode-auth work).**
- `src/kb/enroll.c` — opaque single-use **enrollment tokens** (256-bit, only `sha256`
  stored, constant-time checks), the `aimee://` connection string, an internal **CA**
  (load-or-create + fingerprint), and **client-cert issuance** by redeeming a CSR.
- `src/kb/auth_oidc.c` — a **BYO OIDC / JWT (RS256)** verifier against an operator-
  configured JWKS, mapping verified claims → **scope** (rejects `none`/HMAC; verify-then-
  trust). Pluggable via the `kb_verifier.h` seam. **Note:** this seam verifies **bearer/JWT**
  scopes; **mTLS client certs are validated separately** at the TLS layer
  (`kb_tls_serve.c`), *not* through `kb_verifier.h` — this matters for revocation (§3).
- Scope lattice (`kb_scope.h`) + cross-scope authorization in `kb_http.c`
  (`kb_scope_authorized`, verify-then-trust); **owner-credential vs scoped-credential**
  distinction gating owner-only routes.
- Routes today: `POST /v1/enroll` (owner mints), `GET /v1/enroll/ca`,
  `POST /v1/enroll/redeem`, `POST /v1/enroll/renew` (mTLS). Static bearer *or* scoped
  `scope:<kind>:<id>:<secret>` tokens.
- **Gaps this proposal must close:** no **list / revoke** of issued enrollments or certs;
  the enrollment store keeps only `sha256(token)`, so the **data model** for listing
  (cert fingerprint/serial, scope, state, issued/last-seen/expiry) must be specced +
  migrated (§3); no read/write surface for the live **OIDC config**; no **scope-read**
  surface for the lattice UI.

**The governance data — already built (governance-decision-records work, S1–S7 merged).**
- `decision_log` (`src/db2/schema.sql:49`) — decision records: `options, chosen, rationale,
  assumptions, outcome, status, subject, author, linked_policy_id, supersedes_id,
  revisit_when`; **one-active-per-scope** enforced by
  `idx_dl_active_scope (subject, linked_policy_id) WHERE status='active'`.
- `audit_events` (`src/db2/schema.sql:526`) — the per-action **policy-verdict audit**.
- Curator **review queue** already served at `GET /v1/review` +
  `POST /v1/review/{id}/{accept,reject}` (a **curator-scope** action, not owner — see §4).
- **Gap:** `decision_log`/`audit_events` have **no `/v1` read surface**; the decision
  **write path** (governance S4) is not confirmed to be mounted on the KB `/v1` — this
  proposal resolves it explicitly (§4) rather than "reuse if reachable".

**The dashboard telemetry — mostly already served.** `GET /v1/ingest/status`,
`/v1/corpus/pipeline/status`, `/v1/pipeline/status`, `/v1/workers`, `/v1/contradictions`,
`/v1/code/project-stats`, `/v1/code/projects`, `/v1/releases`(+`/active`),
`/v1/intelligence/calibration/readiness`, `/v1/health`, `/v1/version`, `/v1/capabilities`.
The Dashboard is mostly wiring behind **one** aggregate endpoint (§2).

## §1 The service — `aimee-kb-console`

A new Go thin-client mirroring `aimee-webchat`:

- Serves a **second SPA build** (a distinct entry/router inside `frontend/`, reusing the
  vendored `@rakuensoftware/smoothgui` tarball + Vite config → its own inlined
  `dist/index.html`). Reuses SmoothGUI's shell, nav, and `auth` package (subject to the
  adapter caveat in §0).
- Proxies `/api/*` → **KB** `/v1` (default `http://aimee-kb:8741`). Keeps a SQLite
  **session** store only. Auto-generates TLS.
- **Packaging:** ships in the KB-only image (`Dockerfile`) and `compose.yaml` as an
  additional process/container, on its own port, **binding to localhost/private interface
  by default**. **Default-off / opt-in** (a shared KB is a sensitive surface).
- **Fail-fast startup check:** on boot the console probes every required KB endpoint
  (including the new S2/S3 ones) and refuses to start with a clear error if any is missing —
  so a console build never runs against a KB that lacks its backend.

### §1.1 Security & trust model (resolved — was Open Question #1)

The console is a **privilege-escalation surface** and is treated as **fail-closed**, not
merely "semi-trusted". The roundtable's central finding was to resolve the owner-credential
risk *now*, in the design, not defer it. Resolutions:

- **Scoped console-admin credential, not full owner (decided).** At enable time the operator
  mints a dedicated `scope:console-admin` enrollment for the console with a **bounded route
  allowlist**: `{enroll list/revoke, OIDC config r/w, decisions r/w, audit read, scope read,
  review-queue accept/reject}`. The console uses **that**, never the full owner credential.
  A console compromise is then bounded to that allowlist, not full KB takeover (which would
  otherwise include arbitrary enroll minting, OIDC reconfig, and audit tampering). The KB
  enforces the allowlist server-side; the console's Go role-gate is defence-in-depth, not the
  only line.
- **Deny-by-default proxy.** `/api/*` is an explicit route allowlist mapped 1:1 to permitted
  KB routes — never a generic pass-through — to close the confused-deputy path.
- **Login = OIDC primary; owner-bearer = break-glass only.** Primary login is BYO-OIDC (an
  `admin` claim → console access). A static owner bearer is accepted **only** as a bootstrap/
  break-glass path, logged, with a forced short session expiry — not a routine login.
- **Role verification.** The console verifies the logged-in principal's role from the OIDC
  claim (or the break-glass bearer's fingerprint) on **every** mutating `/api/*` call before
  spending the console-admin cred; read pages may be exposed to a lower scope, mutations
  require the admin role.
- **Credential storage contract.** The console reads its console-admin cred from a **file**
  (`KB_CONSOLE_CRED_FILE`), mode **0600**, owned by the console user; it **refuses to start**
  if the file is group/world-readable and **disallows** an inline env-var credential.
- **Session hardening.** Cookies `HttpOnly; Secure; SameSite=Strict`; **anti-CSRF token** on
  every mutating `/api/*` call (validated console-side before forwarding); short **idle +
  absolute** timeouts (tighter than webchat's); rotate the session id on login; **revoking a
  principal's enrollment/cert invalidates its console sessions**. The SQLite session store is
  the high-value target: mode 0600, at-rest protection (fs-level or SQLite cipher), and each
  session record is **bound** to the OIDC `sub` / bearer fingerprint so a lifted DB row is not
  portable.
- **Console-admin action audit (designed in, not bolted on).** Because the console calls the
  KB with one shared console-admin identity, `audit_events` alone cannot tell *which operator*
  acted. Every state-changing console action (mint, revoke, OIDC write, decision authoring,
  review accept/reject) records the **verified login principal** as the actor, plus route,
  target id, before/after (where safe), source IP/session, verdict, and a correlation id —
  persisted via a new `audit_events` event-kind (or sibling table). This is a **hard
  requirement of S2/S3**, gated before those actions ship.
- **CSP.** The admin SPA is served with a **strict Content-Security-Policy** (no remote
  origins; scripts limited to the inlined bundle via hash/nonce) even though webchat's posture
  is looser — an XSS here would exfiltrate proxied minted tokens and decision content.

## §2 Dashboard page (read surface)

A SmoothGUI dashboard behind **one committed aggregate endpoint**
**`GET /v1/console/overview`** (in S1, not optional): the console fans the ~12 telemetry
sources in server-side into **one versioned, timestamped, partial-failure-aware** response
with per-component degraded states and a bounded timeout — avoiding 12×N owner-cred round
trips and inconsistent client-side snapshots. Panels: **Pipeline** (ingest / curator / worker
queue depth + throughput), **Knowledge** (memory / entity / vector-collection / code-index
counts), **Health** (active collection, calibration readiness, contradictions), **Version/
Capabilities**. No new tables.

## §3 Accounts (backend gaps first, then UI)

**Backend (new, owner/console-admin-gated, all with pagination caps + parameterized filters
+ per-principal & per-IP rate limits):**

- **`GET /v1/enrollments`** — list issued enrollments/certs: id, scope, state (pending /
  redeemed / revoked / expired), **cert fingerprint/serial**, issued / last-seen / expiry.
  Requires first defining the **enrollment/cert data model** and a migration (today only
  `sha256(token)` is stored); include the last-seen update path. Resource-oriented naming
  (`/v1/enrollments`, `POST /v1/enrollments/{id}/revoke`) rather than verb-suffixing the
  existing `/v1/enroll/*` bootstrap routes.
- **`POST /v1/enrollments/{id}/revoke`** — mark revoked. **Two enforcement points** (they are
  distinct code paths): (a) revoked **cert fingerprints** rejected at the **mTLS handshake /
  request-auth** layer (`kb_tls_serve.c`), and (b) revoked **bearer scopes** rejected in the
  scope-authorization path — *not* only "the verifier seam", which is OIDC/JWT-only. Specify
  the lookup mechanism (in-memory LRU keyed by enrollment id / fingerprint, short TTL, fed by
  a notifier from the revoke write) and document the residual revocation window.
- **`GET /v1/scopes`** — the scope lattice + which enrollments/principals hold which scope
  (the read surface the lattice UI needs; absent today).
- **OIDC config r/w** — **DB2-backed** (resolves OQ#4): a config row + `GET/PUT
  /v1/config/oidc`, atomic read/write, propagated live via
  [[live-config-reload-initiative]] (no KB restart). **Guarded write semantics** because a
  bad JWKS/iss/aud/claim-map can lock out every admin:
  - **validate by fetching the configured JWKS server-side** (with SSRF guards on the URL),
    confirming iss/aud and that ≥1 key resolves — *not* a single crafted sample token;
  - **versioned** config with **auto-rollback** if no successful auth lands within a window
    after apply; and a **break-glass** owner-bearer/mTLS path (documented runbook, S4) that
    bypasses OIDC for recovery.

**UI:** enroll-a-client (render token + `aimee://` + CA, copy-to-clipboard); the
enrollments table with revoke; scope-lattice view; OIDC config editor with the dry-run +
rollback affordances. Surface the operator's **effective scope** on every action button.
*Renew*: clarify it is an **operator action via the console-admin cred** (or a link-out
runbook) — a browser session does **not** hold the client cert that `POST /v1/enroll/renew`
mTLS-renews, so the UI does not call the mTLS renew directly.

## §4 Governance (backend gaps first, then UI)

**Backend (new reads, paginated + parameterized + free-text redaction; reads scoped by
principal, not merely "logged in"):**

- **`GET /v1/decisions`** (filter by subject / status / scope) and **`GET /v1/decisions/{id}`**
  (detail + supersede chain via `supersedes_id`), surfacing `decision_log` and the
  one-active-per-scope status.
- **Decision write path (resolved, not conditional):** confirm whether the governance-S4 write
  path is mounted on the KB `/v1`; if not, this proposal **specs the route** here —
  create / supersede / set-outcome / set-status / revisit — with: authorization
  (`scope:decisions:write` on the subject), the **one-active-per-scope conflict response**
  (the index stays authoritative; the API returns a typed conflict, never bypasses it), and
  idempotency behavior. Decision write lands in the governance **backend** sub-slice.
- **`GET /v1/audit/actions`** — paged policy-verdict feed over `audit_events`, **mandatory
  time-window filter** (no full-table scans), redacting free-text where required.

**UI:** decision-records browser + authoring; the action-audit feed; and the **review queue**
— but review accept/reject is a **curator-scope** action (`scope:curator:write` on the
existing `/v1/review`), **split out of the owner/admin gate**, with the effective scope shown
on each button.

## Slices (backend-before-UI; each roundtable-gated per house workflow)

Reordered per the roundtable — the containment model and the backend gaps land before any UI
that depends on them.

- **S0 — foundation + containment.** Scaffold `aimee-kb-console` (Go service + second
  SmoothGUI SPA shell, default-off, localhost-bound). **Acceptance criteria include the
  trust-model doc, the scoped console-admin credential + deny-by-default allowlist, CSRF +
  session hardening, CSP, the cred-file 0600 contract, and the SmoothGUI-auth adapter check.**
  (These are S0 gates, not S4 afterthoughts.)
- **S1 — Dashboard.** `GET /v1/console/overview` aggregate + the dashboard UI.
- **S2 — Accounts backend.** `/v1/enrollments` (+ data model & migration),
  `/v1/enrollments/{id}/revoke` (both enforcement points), `/v1/scopes`, DB2-backed
  `/v1/config/oidc` (guarded writes + rollback + break-glass) + console-admin audit for each;
  auth/authz tests.
- **S3 — Accounts UI.**
- **S4 — Governance backend.** `/v1/decisions*` (incl. the resolved write path + conflict
  response), `/v1/audit/actions`; console-admin audit; tests.
- **S5 — Governance UI** (incl. curator-scoped review queue).
- **S6 — Hardening close-out.** Packaging/smoke, break-glass runbook, feature doc, default-off
  verification.

## Open questions for the roundtable (remaining)

1. **OIDC config storage** — resolved to **DB2-backed** with a hard dependency on
   [[live-config-reload-initiative]]; confirm that dependency is acceptable, or fall back to a
   file-config + restart for v1.
2. **Multi-KB** — **deferred**: one console per KB for v1 (a KB-selector + per-KB cred
   management is a future proposal). Confirm.
3. **Default-on posture** — stays default-off; a later operator-enable proposal (like the
   governance / full-autonomous flips) owns the flip.

### Resolved by the 2026-07-04 roundtable (recorded)
Owner-cred → scoped console-admin + allowlist; trust-model/CSP/CSRF moved into S0; slices
reordered backend-before-UI; console-admin action-audit made a hard S2/S4 requirement;
revocation split into mTLS-handshake + scope-path enforcement; enrollment/cert data model +
migration called out; decision write-path spec made explicit (not conditional); review queue
split to curator scope; pagination/rate-limit/redaction required on all new list endpoints;
`/v1/console/overview` committed in S1; OIDC guarded-write (JWKS-fetch validation, versioned +
auto-rollback + break-glass, SSRF guard); login OIDC-primary / owner-bearer break-glass;
resource-oriented endpoint naming.
