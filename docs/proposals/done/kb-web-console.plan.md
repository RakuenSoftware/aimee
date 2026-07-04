# Implementation Plan: aimee-kb web console

Companion to `kb-web-console.md` (merged to `testing`, PR #1058). Grounded against the code
2026-07-04; **plan-roundtabled 2026-07-04 (v2 — 3 blocking + top findings folded in)**.
Backend-before-UI ordering; each slice is independently shippable and default-off, so nothing
here changes a default deployment until the S6 close-out (and even then the console stays
opt-in).

## Verification substrate

- **Inner loop — kb build + unit tests.** Production build is `make -C src ../aimee-kb`
  (the CMake targets are known-stale — [[aimee-two-build-systems]]; use `make`). Pure C
  helpers (route-ACL match, canonicalization, hashing) get `ctest` unit targets via the
  `aimee-core` static lib **and** are compiled into the `make` production build (same code in
  both graphs — no CMake-only helper that the shipped binary never links).
- **Console service loop — Go.** New `kb-console/` module, `CGO_ENABLED=1 go build` (cgo for
  sqlite; **no PAM**). `go test ./...` for the proxy allowlist, CSRF, session, role-gate,
  OIDC-verify parity.
- **Frontend loop — Vite.** `cd frontend && npm run build`; a second single-file SPA entry.
- **Integration loop — PVE `root@192.168.1.253`.** Fresh `aimee-docker` CT for any slice
  touching live DB2/postgres + a running kb (S2a/S2b, S4). Env/PATH bug workaround
  ([[lxc2docker-image-env-path-bug]]). `.254` = shared roundtable/LLM endpoint, not a build host.
- **CI gates** — no co-author trailers / no Claude attribution
  ([[aimee-ci-no-coauthor-trailers]], [[no-claude-attribution-in-prs]]); schema/docs-gen/lint/
  Windows gates ([[aimee-pr-ci-gates]]). **Any `/v1` route add regenerates the KB OpenAPI
  contract** (`api/openapi-v1.yaml` → `docs/gen/api-v1.md`) in the SAME slice that adds it —
  including the S0 `/v1/console/overview` stub — or the conformance check fails.

## Per-slice protocol

Each slice: `git fetch && git checkout -B kbconsole/<slice> origin/testing` in the
`/home/virant/dev/aimee-kb-console` worktree → implement → inner-loop build+tests (+ .253
integration where noted) → **roundtable review the CODE** ([[always-roundtable-review-before-pr]];
single-persona-panel substitute if the diff is ≥~40 KB — [[roundtable-invocation]]) → address
findings → PR to `testing` → merge. State in [[kb-web-console-initiative]].

## Grounded seams (verified 2026-07-04)

- **KB HTTP dispatch** — `src/kb/http/kb_http.c`: `strcmp(path,…)` chain after the verifier;
  `kb_verifier_result vr` carries `scope_kind`/`scope_id`; `kb_http_owner_required(out,cap,what)`
  is the shared owner-only 403; `POST /v1/enroll` already rejects a scoped bearer through it.
  New surfaces get sibling modules (`kb_http_console.c`, `kb_http_governance.c`) registered from
  `kb_http.c`.
- **Route ACL is method+route-id, not path-prefix.** New `src/kb/http/kb_route_acl.{h,c}`: a
  **static compiled** table mapping `scope_kind → set of (method, route-id)` where route-id is
  the matched canonical route (assigned during dispatch), NOT the raw request path — so encoded
  paths, trailing slashes, query strings, wrong methods, and sibling paths like
  `/v1/enrollments/foo/revoke/extra` cannot slip through. Static (compile-time) — no DB/reload
  dependency; updating the table is a deployment-time change, documented.
- **Enrollment store** — `src/kb/enroll.c` (+ `kb_enroll.h`): tokens 256-bit, **only
  `sha256(token)` persisted**; CA + CSR issuance present; **no list/revoke and no redeemed-cert
  record today.** S2a adds the persisted row + migration (below).
- **mTLS vs bearer are separate seams** — client-cert validation is at the TLS layer
  (`kb_tls_serve.c`, `kb_tls.c`), NOT through `kb_verifier.h` (the OIDC/JWT/scoped-bearer seam,
  `auth_oidc.c`). Revocation is enforced in **both**.
- **OIDC verifier config** — `auth_oidc.c` loads JWKS/iss/aud/claim-map. S2b makes it
  DB2-backed (`kb_oidc_config`) behind `GET/PUT /v1/config/oidc`, live-applied via
  [[live-config-reload-initiative]] (merged #1056).
- **Governance data** — `decision_log` (`schema.sql:49`; one-active-per-scope
  `idx_dl_active_scope` — a partial UNIQUE index `WHERE status='active'`) + `audit_events`
  (`schema.sql:526`); DB2 API + `kb_client_*` wrappers exist.
- **Dashboard telemetry (served)** — `/v1/ingest/status`, `/v1/corpus/pipeline/status`,
  `/v1/pipeline/status`, `/v1/workers`, `/v1/contradictions`, `/v1/code/project-stats`,
  `/v1/releases`(+`/active`), `/v1/intelligence/calibration/readiness`, `/v1/health`,
  `/v1/version`, `/v1/capabilities`.
- **GUI template** — `webchat/*.go` + `frontend/` (React 19 + Vite `viteSingleFile`, vendored
  `@rakuensoftware/smoothgui`). `smoothgui/auth v0.2.3` is PAM-shaped (`RunPAMHelper`) — the
  console does NOT reuse it for login.

## Auth & containment (the S0 spine — resolved)

- **Console-admin credential.** Enable-time the operator mints a `scope:console-admin`
  enrollment (`POST /v1/enroll`) whose route ACL (static, in `kb_route_acl.c`) =
  `{GET /v1/console/overview, GET /v1/enrollments, POST /v1/enrollments/{id}/revoke,
  GET+PUT /v1/config/oidc, GET /v1/scopes, GET /v1/decisions, GET /v1/decisions/{id},
  POST /v1/decisions(+sub-actions), GET /v1/audit/actions}`. **`/v1/review` is intentionally
  NOT in this set** — review accept/reject is `scope:curator:write` (a *separate* cred; see
  S4/S5), so console-admin does not silently imply curator powers. The KB enforces the ACL
  server-side; the Go role-gate is defence-in-depth.
- **Revocation source of truth = DB2** (`revoked_at` on the enrollment row). The per-instance
  in-memory check is a **read-cache** (LRU, short TTL, invalidated by a notifier from the revoke
  write); a stale cache self-heals within the TTL, which is the documented **residual window**.
  Multi-node coherence therefore needs no gossip — every node reads the same DB row on cache
  miss. Single-instance is the documented default; the TTL bounds the multi-node window.
- **Login = OIDC primary.** The Go console verifies a browser-presented OIDC JWT itself using
  **`golang-jwt/jwt/v5`** (vendored via `go mod vendor`, committed — satisfies the no-network
  build rule). Verifier acceptance criteria (parity-tested against `auth_oidc.c`): RS256 only
  (reject `none`/HS/alg-confusion), `kid` selects the JWKS key, check `iss`/`aud`/`exp`/`nbf`
  with bounded clock skew, JWKS cache TTL + refresh-on-miss, and the **admin claim** is pinned
  (claim name + accepted values, same source-of-truth mapping as `auth_oidc.c`; a **parity
  test** fails if the two maps diverge). **S0 reads OIDC settings from a read-only file/env**
  (issuer/aud/JWKS/claim-map) since the editable `/v1/config/oidc` doesn't exist until S2b.
- **Break-glass login.** A static owner/console-admin bearer login is enabled ONLY when a
  presence-on-disk flag file (`$KB_CONSOLE_HOME/.break_glass`, mode 0600) exists; **off by
  default.** Every break-glass login: session capped at **300s**, and an audit row written to
  **both** the console-local SQLite and the KB `audit_events` (operator-visible severity). Used
  when OIDC is unconfigured/misconfigured (the lockout recovery path).
- **Deny-by-default proxy.** `/api/*` is an explicit map to the ACL'd KB routes; no generic
  pass-through.
- **Session + CSRF.** `HttpOnly; Secure; SameSite=Strict` cookie; per-session anti-CSRF token
  on every mutating `/api/*`; idle + absolute timeouts tighter than webchat; session id rotated
  on login; session row bound to **`(iss, sub)`** (not `sub` alone — avoids cross-IdP `sub`
  collision) / bearer fingerprint; SQLite session DB mode 0600, at-rest protected. **Revoking a
  managed client enrollment invalidates its console sessions; revoking the console's OWN cred**
  makes the console stop proxying immediately, fail its health/startup probe, invalidate all
  sessions, and require operator reconfiguration (integration-tested).
- **CSP** strict on the SPA: `default-src 'self'`; the only non-self entries are the IdP
  domain(s) in `form-action`/`frame-src`/`connect-src`, computed from an explicit list (never
  `*`); asserted by a frontend test. **Cred file** `KB_CONSOLE_CRED_FILE` mode 0600 — refuse to
  start if group/world-readable or if an inline env cred is set.
- **What is reused from webchat vs NOT** (documented in `kb-console/README.md`): REUSE — the
  HTTPS auto-TLS server scaffold, SQLite session-store shape, `/api/*` proxy shape, single-file
  `dist/` embed. NOT reused — `RunPAMHelper`, any `pam_*` import, webchat PAM/session helpers.

---

## S0 — console scaffold + containment model

**Backend (C, additive, gated):**
- `kb_route_acl.{h,c}` (new) + `console-admin` scope kind in `kb_scope.h` (after reviewing
  existing kinds for overlap); enforce the ACL in the `kb_http.c` dispatch after route match.
- `GET /v1/console/overview` **stub** (`{version, generated_at, components:[]}`) in
  `kb_http_console.c` — a real ACL'd route to gate against; **regenerate the OpenAPI contract in
  this slice.**

**Console service (Go, new `kb-console/`):** auto-TLS HTTPS, SQLite sessions, `/api/*`
deny-by-default proxy using the console-admin cred, OIDC-verify (`golang-jwt/jwt/v5`, file-config
in S0) + presence-flag break-glass, CSRF + session hardening (`(iss,sub)` binding), strict CSP,
cred-file 0600 contract, fail-fast startup endpoint probe. Serves the second SPA. Vendored deps
committed.

**Frontend:** `frontend/console.html` + `frontend/src/console/{main,ConsoleApp}.tsx` (SmoothGUI
shell + nav, placeholder Dashboard/Accounts/Governance routes) + `vite.console.config.ts` →
`frontend/dist-console/index.html`. Reuses the vendored SmoothGUI; no PAM.

**Docs:** `docs/KB_CONSOLE.md` trust-model section; `kb-console/README.md` reuse-list.

**Tests:** Go — ACL rejects non-allowlisted `(method, route-id)` incl. encoded/trailing-slash/
wrong-method/sibling paths; CSRF required on POST; role-gate blocks mutation without admin;
`(iss,sub)` session binding; break-glass off unless flag present + dual audit; console-own-cred
revocation path. C — `console-admin` authorized only for ACL routes (ctest, make-linked).

**Default-off:** separate opt-in binary; no compose/Dockerfile wiring until S6.

## S1 — Dashboard

- Fill `GET /v1/console/overview`: fan in telemetry by calling **KB read models in-process**
  (NOT by looping back through the HTTP auth layer — the console-admin ACL would self-deny the
  underlying telemetry routes). Concurrent fan-in (errgroup-style) with **per-source timeout
  (~1.5s)** + **global deadline (~3s)**; each component `{name, ok, degraded, data|error,
  fetched_at}`; envelope `degraded:true` iff ≥1 source errored/timed out; versioned + timestamped.
  Regenerate OpenAPI.
- Dashboard page: Pipeline / Knowledge / Health / Version panels; per-component degraded render.
- Tests: overview returns partial success when one source errors + on timeout; a console-admin
  bearer can call `/v1/console/overview` but still cannot call the underlying telemetry routes
  directly; Vitest render vs fixture. Integration: .253 CT.

## S2a — Accounts backend: enrollments / revoke / scopes (no live-reload dep)

- **Enrollment/cert model + migration** (`schema.sql` + `enroll.c`): persist the enrollment/
  redeemed-cert row (id, scope, state, fingerprint/serial, issued, **last-seen**, expiry,
  `revoked_at`). Migration **idempotent** (`CREATE TABLE/ALTER … IF NOT EXISTS`) with a
  schema-version pre-check. **Legacy creds** (issued before this row existed): backfill
  fingerprint/serial on first successful request and mark `legacy=true`; console revoke/list
  operate on them once backfilled (documented; no silent gaps).
- **`last-seen` is debounced** — updated only on auth-establishing events and at most once per
  configured interval (e.g. 5 min) for steady traffic; best-effort (a failed update never blocks
  auth). No per-request DB writes.
- `GET /v1/enrollments`, `POST /v1/enrollments/{id}/revoke` — paginated (default/max caps),
  parameterized filters, **numeric rate limits** (mint 5/min·principal, 20/min·IP; revoke
  30/min·principal; single-instance limiter documented as a constraint). Revocation enforced at
  **(a)** the mTLS handshake/request-auth (fingerprint, `kb_tls_serve.c`) and **(b)** the scope
  path; DB `revoked_at` is source of truth + LRU read-cache + notifier (residual window = TTL).
- `GET /v1/scopes` — the lattice + holders.
- **Console-admin action audit**: each mint/revoke records the verified principal (new
  `audit_events` kind), actor/route/target/before-after/ip/correlation-id.
- Regenerate OpenAPI. Tests: revocation actually rejects at **both** seams; legacy backfill;
  debounced last-seen; audit row; ACL. Integration on .253.

## S2b — Accounts backend: DB2-backed OIDC config (gated on live-config-reload)

**Pre-S2b go/no-go:** confirm the [[live-config-reload-initiative]] re-applier registry is merged
+ exercised against a non-OIDC consumer. If not ready, **fall back to file-config + documented
restart** for v1 (rollback/dry-run then mean "validate then write file + operator restarts");
record the choice in S6 close-out.

- `kb_oidc_config` row + `GET/PUT /v1/config/oidc`. **Guarded write:** fetch the configured JWKS
  server-side with concrete **SSRF guards** — HTTPS-only; reject loopback/link-local/RFC1918/ULA/
  multicast (unless explicitly allowlisted); re-resolve + re-check IP after any redirect (DNS-
  rebind defense); response size cap (256 KiB); per-fetch timeout; JSON-schema validate
  (`{keys:[JWK…]}`); no credentials sent; audit attempted + accepted URLs. Confirm `iss`/`aud` +
  ≥1 key resolves before persist.
- **Versioned config + attributed auto-rollback:** stamp the active config **version** on every
  auth event; roll back only when **zero NEW-version auths** are observed for N minutes after a
  switch (so rollback can't mis-fire on old-config traffic). Live-applied via the re-applier
  registry (or restart in the fallback). Console-admin audit on writes.
- Regenerate OpenAPI. Tests: SSRF guard rejects private/redirect/oversize/non-JWKS; bad-config
  keeps the old version; version-attributed rollback fires only on new-version starvation.
  Integration on .253 (incl. the lockout→break-glass recovery drill).

## S3 — Accounts UI

Enroll-a-client (token + `aimee://` + CA, copy); enrollments table + revoke; scope-lattice view;
OIDC config editor with dry-run (server validate) + rollback affordance + effective-scope badges;
renew clarified as an operator/link-out action (browser holds no client cert). Vitest render tests.

## S4 — Governance backend

- `GET /v1/decisions` (filter subject/status/scope) + `GET /v1/decisions/{id}` (supersede chain).
- **Decision write path — resolved upfront (pre-S4 spike, ~½ day):** either confirm the
  governance-S4 writer is already mounted on the KB `/v1` (and list the routes) OR add, as
  explicit sub-tasks: **S4a** `POST /v1/decisions` (+ `supersede`/`set-outcome`/`set-status`/
  `revisit`) with `scope:decisions:write` authz + **`Idempotency-Key`** (IETF draft header; TTL
  24h; DB2 row keyed `(principal, key)` storing the response; same key + different payload → 409),
  and a typed **one-active-per-scope conflict (409)** response (the partial unique index stays
  authoritative — the API surfaces the conflict, never bypasses it); **S4b** `GET
  /v1/audit/actions` (paged, **mandatory time-window**, parameterized filters, free-text
  redaction, principal-scoped reads); **S4c** the console-admin audit on decision writes.
- **Provision `scope:curator:write`** here (add to `kb_scope.h`/lattice + minting) so S5's review
  UI gates against a scope the verifier actually knows — no UI-only reference to a missing scope.
- Regenerate OpenAPI. Tests: conflict 409 on a second active decision for a scope; idempotency
  replay; audit pagination caps + redaction + mandatory window. Integration on .253.

## S5 — Governance UI

Decision-records browser + authoring form (surfaces the 409 conflict, never bypasses the
invariant); action-audit feed with the mandatory time-window; review queue gated at
`scope:curator:write` — the console presents a **separate curator-scoped credential** for
`POST /v1/review/{id}/{accept,reject}` (NOT the console-admin cred), effective-scope on each
button. Vitest render tests.

## S6 — Hardening close-out

- **Packaging:** add the console to the KB image (`Dockerfile`) + `compose.yaml` as an opt-in,
  localhost-bound, **default-off** service via `profiles: [console]` (a stock `compose up` does
  NOT start it) + a `KB_CONSOLE_ENABLED` guard; the second SPA build stage. **CI gate** asserting
  a default `compose config` omits the console service.
- Break-glass + OIDC-lockout runbook; `docs/KB_CONSOLE.md` completion; ARCHITECTURE.md
  "§10 the kb console" entry; record the S2b live-reload-vs-restart choice.
- Live smoke on a .253 CT: enable console, OIDC login (+ break-glass drill), dashboard,
  mint/revoke (both revocation seams), a decision (+ conflict), the audit feed — end to end.
- Move `kb-web-console.md` + this plan to `docs/proposals/done/` with a close-out header.

## Resolutions folded from the 2026-07-04 plan roundtable
Revocation source-of-truth = DB2 row + LRU read-cache (residual window = TTL; single-instance
default); break-glass = presence-flag, 300s, dual audit sink; route ACL = static `(method,
route-id)` table in `kb_route_acl.{h,c}` after route-match; `/v1/review` removed from
console-admin (curator scope provisioned in S4, used in S5); overview fans in **in-process**
(no HTTP self-loop) with per-source + global deadlines; **S2 split** into S2a (enroll/revoke/
scopes, no live-reload dep) + S2b (OIDC config, gated + file-restart fallback); S0 OIDC =
read-only file-config + OpenAPI regen for the stub; Go verifier = `golang-jwt/jwt/v5` vendored +
parity test vs `auth_oidc.c` + pinned admin claim; JWKS SSRF concrete guards; version-attributed
auto-rollback; debounced last-seen; numeric rate limits; `Idempotency-Key` on decision writes;
session bound to `(iss,sub)`; idempotent migration + legacy-cert backfill; console-own-cred
revocation behavior; C helpers linked in both make + ctest graphs.
