# Proposal: complete credential-vault consolidation

- **State:** DONE — rev. 3 implemented across P1–P4 + a 2026-06-28 closeout (PRs #833/#834/#836/#837);
  see §Closeout. (rev. 3 history: R1+R2 reviewed → revised; fleet can only reliably review a doc this
  size via one model — see §Review history; USER proposal-approval was gate 1.)
- **Status refreshed:** 2026-06-28
- **Author:** JBailes
- **Date:** 2026-06-14
- **Builds on:** [delegate-refactor-async-and-credential-vault](../done/delegate-refactor-async-and-credential-vault.md)
  (the envelope-vault + server-sealed dual-access core, WP-A..WP-C.4, already merged
  to `testing` and **deployed live on `.254` in `aimee-server v0.2.68`**).
- **Charter roles:** Secure (one encrypted-at-rest home for every credential,
  fail-closed), Delegate (every delegate authenticates from the vault), Gate-Promote
  (default-safe migration with coexistence + rollback).

> **Revision note (R1).** Reviewed by a diverse delegate panel; security returned
> **BLOCKED**, reviewer **CONCERNS** (architect/engineer lenses were lost to a provider
> 429 + a reasoning-model stall and are owed in R2). Three convergent findings drove
> rev. 2: (1) **server-principal writes were under-protected** — bare `/v1` bearer over
> plaintext `0.0.0.0:8740` could mint server-owned creds (trust-boundary collapse); rev. 2
> gates them behind a dedicated capability + per-write audit + a transport requirement
> (D2/D2b). (2) **Migration/retirement had no atomicity or rollback**; rev. 2 adds a
> migration state machine, a per-agent verifier, a backup-before-scrub rule, and a server
> `vault_only` rollback flag (WP-3/WP-4). (3) **Codex refresh was inconsistent and
> under-specified**; rev. 2 consolidates codex onto the existing `oauth_tokens.c` refresh
> layer with a named schema + an expired-refresh→device-flow fallback (WP-2). The four
> open forks are now decided (§Forks). The security seat's "server is v0.2.58 / no vault"
> finding was a **sandbox-blind misread** (it could not curl the live endpoints): the
> **running** server authoritatively reports `{"version":"v0.2.68"}` on `/v1/version`, and
> an absent `.vault/` is the *expected* pre-first-write state. §0 is re-confirmed below.
>
> **Revision note (R2).** rev. 2 re-reviewed; security **CONCERNS**, engineer **BLOCKED**;
> the architect/reviewer lenses were lost again (mimo-2.5 context-capped on the larger doc —
> see §Review history). Both seats converged on three *residual* gaps, now closed in rev. 3:
> (B1) the `vault:write:server` capability had **no provisioning/revocation model** and D2b's
> check was **IP-based** (tunnel-bypassable) — rev. 3 commits an operator-minted, UDS-only
> grant + binds the transport check to **attested transport, not IP**, names the audit sink +
> a key **fingerprint** (never the key), and bounds this proposal to **exactly one** capability
> (D2/D2c). (B2) the `vault_only` **blast radius was not enumerated**, the per-agent state
> machine lacked a **lock + a vault-level verifier**, and the agent set was hard-coded — rev. 3
> adds a discovery step, a per-agent lock, a decrypt-roundtrip verifier, a **dry-run** that
> lists every surface that would 401, and a hard cutover gate (WP-3/WP-4). (B3) the codex
> "device-flow fallback" was a **misnomer on an unattended server** and lazy-migration lacked
> atomicity — rev. 3 states plainly that a revoked refresh token raises an operator
> "re-auth required" + an admin `codex reauth` command (no autonomous browser flow), makes the
> migration **atomic (verify-then-scrub)**, and keys `oauth_tokens.c` by **(principal, service,
> cred)** (WP-2). The R2 seats re-flagged the v0.2.58 banner — see §Evidence; stale prior-boot
> log line, now pinned.

## Closeout (filed to done — 2026-06-28)

rev. 3 was USER-approved (gate 1) and implemented across P1–P4 (PRs #291/#294/#298/#305/#306,
promoted in #311; codex vault-refresh #724). A 2026-06-28 closeout audit + roundtable review found
six divergences from the written plan; the must-build set was completed and merged:

- **D2/D2c** — every server-principal credential write **and** `vault:write:server` capability
  grant/revoke now records to the dedicated append-only **0600 `audit.log`** sink (tamper-evidence +
  access separation) instead of the operator-readable general server log. **PR #833.**
- **D13** — the `.server-master.key` rotation shipped as the **offline `aimee-server
  --rotate-master-key`** (re-wrap not re-encrypt; backup-before-mutate + restore-on-fail;
  server-stopped guard; symlink-safe copy; post-rewrap read-back verify) plus
  [`docs/runbooks/vault-master-key-rotation.md`](../../runbooks/vault-master-key-rotation.md).
  **PR #834.**
- **D9** — `agent key import` gained **backup-before-scrub** (atomic 0600, `O_EXCL`/`O_NOFOLLOW`) +
  a per-run lock. The heavier scaffolding (per-agent `NOT_STARTED→…→LEGACY_SCRUBBED` state machine,
  vault-level decrypt-roundtrip verifier, `vault status` migration-incomplete surfacing) is a
  **tracked follow-up**: the roundtable agreed the decrypt-roundtrip verifier's rationale — defeating
  a false-VERIFIED via a still-live `/v1/session/credentials` — is moot now that path is deleted, and
  backup-before-scrub covers the remaining durability failure mode. **PR #836.**
- **D6** — codex refresh now distinguishes an IdP **`invalid_grant`** rejection (→ a persistent
  `REAUTH_REQUIRED` vault marker + an explicit delegate error) from transient errors, with an
  operator-attended **`aimee codex reauth`** command (no autonomous browser flow). **PR #837.**

**D10 — superseded (operator sign-off recorded).** The `vault_only` rollback flag + per-surface
`--dry-run` + N-day cutover gate were a coexistence-management lever for the legacy client-push path.
P4b **deleted that path outright** (D11, stronger than the planned opt-in read-fallback), so the flag
has nothing left to gate; the intent (vault as the sole live source, provider `$ENV` as the one
by-design fallback) is met more strongly by removal. **Operational note:** emergency rollback is now a
code-revert + redeploy, not a runtime switch — a deliberate loss of a runtime blast-radius lever,
accepted at closeout.

**D12 — intentional carve-out.** The codex CLI owns and writes its own `~/.codex/auth.json`; aimee
reuses it as the vault-bootstrap source and a last-resort fallback. The vault is the **top** resolution
tier and the sole **aimee-managed** store, but codex's self-managed auth file is an **intentional
external source**, not a vault miss. "Vault is the sole source" holds for every aimee-stored
credential; codex's CLI-owned file is the documented exception.

## Goal

Make the **server-owned vault the single source of every delegate/agent credential**
and retire the legacy plaintext/RAM-push paths. The crypto, the server-sealed
autonomous-decrypt master key, the server principal, the API-key/codex cred slots, the
agent-add→vault handler, and the vault-first use-path **already exist and are deployed**
(§0). What is missing is the *write* side and the *retirement* of the legacy coexisting
paths:

1. **The thin client never sends a literal key to the server vault.** `agent add --key`
   stores the secret client-side and pushes it to server RAM per session — it does not
   reach `vault_service_set*`. (Proven, §0.)
2. **Codex (and the codex-style OAuth device flow) has no vault *write* path.** The token
   slots are read-only today; the device-setup writes only `codex-auth.json` on disk and
   never refreshes.
3. **The legacy paths still coexist** (client `agent-keys.json` + `/v1/session/credentials`
   RAM push; server `codex-auth.json` disk read) and must be retired so the vault is the
   sole source.

Out of scope for the *parent* proposal but in scope here: the client→server forwarding,
the codex/OAuth vault-write + refresh, the one-time migration of existing creds, and the
legacy-path retirement.

## §0 What already exists (verified against deployed `.254` v0.2.68 + `origin/testing`)

- **Vault is built, merged, and LIVE.** The **running** server on `.254` reports
  `{"version":"v0.2.68","service":"aimee-server"}` on `GET /v1/version` (authoritative —
  not just the on-disk binary). Confirmed present in the deployed binary:
  `.vault/.server-master.key`, `/v1/vault/set`, `codex_oauth_token`, `vault locked`, and
  the agent-add refusal string `could not store credential in the vault`. A live
  `aimee vault list` over TCP returns the vault-specific error "this connection has no
  attested local identity" (vault code present; the `uid:` path is TCP-refused by design).
  `.vault/.server-master.key` does **not** exist on disk yet — *expected*: it is created on
  the first server-principal write, which is exactly what WP-1/WP-2 introduce. Modules:
  `src/server/vault_{crypto,kek_cache,principal,server_key,service}.c`, `vault_store.h`,
  `server_vault.c`.
- **Autonomous decrypt works.** `vault_server_kek()` (`vault_server_key.c`) manages a 0600
  `<AIMEE_HOME>/.vault/.server-master.key` (atomic create + fsync) and the **server
  principal** `VAULT_SERVER_PRINCIPAL "server"` decrypts without a human unlock —
  `vault_service_set_server()` / `vault_service_get_server_principal()` (WP-C.4). User
  principals (`uid:`/`webuser:`) get a `vault_store_set_dual` (user KEK + server KEK).
- **The agent-add→vault handler exists** (`server_agent.c:~515`): a *literal* key on
  `/v1/agent/add` is stored via `vault_service_set(principal,…)` if the conn has a per-user
  principal, else `vault_service_set_server(…)`, and the handler **refuses** rather than
  write plaintext to `agents.json`. An `$ENV`-ref key stays unexpanded in `agents.json`.
- **The use-path reads the vault on the PRIMARY path.**
  `delegate_run_with_credential_retry` (`server_compute.c:1377`) calls
  `vault_service_inject_api_key`, which resolves **server-wrap → user-KEK → server
  principal** — the last is an explicit fallback for "delegate creds pushed from a TCP thin
  client, which has no per-user principal." Cred slots: `VAULT_API_KEY_CRED "api_key"`,
  `VAULT_CODEX_TOKEN_CRED`, `VAULT_CODEX_ACCOUNT_CRED`.
- **A generic OAuth token layer with refresh already vaults under the server principal.**
  `oauth_tokens.c` stores `oauth_access_token` / `oauth_refresh_token` / `oauth_expires_at`
  via `vault_service_set_server`, exposes `oauth_token_get()` with **auto-refresh**, and
  **lazily migrates** legacy `db1/secrets` plaintext into the vault on first read, then
  scrubs it. Codex is **not** wired to this layer.

### The gaps (verified)

- **G1 — client skew.** The deployed client (`aimee v0.2.51-94-gfb7615b`) implements the
  legacy client-held model: `cli_agent_keys.c` writes the literal key to
  `~/.config/aimee/agent-keys.json` and pushes it per-session to `/v1/session/credentials`
  (server RAM). It does **not** forward the literal key to `/v1/agent/add`. **Proven:**
  after `agent add glm --key …`, the key reappears in `agent-keys.json`; and a glm delegate
  fails with "Authentication parameter not received" once glm is scrubbed from
  `agent-keys.json` (the server vault is empty). So no API key ever reaches the vault.
- **G2 — codex vault write gap.** `VAULT_CODEX_TOKEN_CRED` / `VAULT_CODEX_ACCOUNT_CRED` are
  **only read** (`delegate_credential_retry.c:31,35`), never written. The codex device-setup
  (`server_agent.c`) writes only `codex-auth.json`; there is no refresh. `.254` currently
  has **no** `codex-auth.json` (and no `~/.codex/auth.json`) → every codex call 401s.
- **G3 — no client path to write a server-principal cred.** `aimee vault set` is the `uid:`
  path and is refused over TCP ("no attested local identity", parent D17). Server-principal
  writes happen only server-side today (agent-add + oauth flows). A migration affordance is
  needed.
- **G4 — `agent setup` targets a local server.** In v0.2.51, `aimee agent setup codex`
  fails "server unavailable" against a thin-client→remote deployment, so codex cannot be
  provisioned onto the remote through the client at all.

## Decisions / work packages

### WP-1 — Client forwards delegate API keys into the server vault

- **D1.** `agent add --key <literal>` **forwards the secret to the server** on
  `/v1/agent/add` so the deployed v0.2.68 handler vaults it under the resolved principal
  (server principal over TCP; `uid:`/`webuser:` where attested). The client **stops** writing
  literal keys to `agent-keys.json`. The `/v1/session/credentials` RAM push is kept only as a
  transitional fallback for not-yet-migrated agents (retired in WP-4).
- **D2 — server-principal writes require a dedicated capability + audit (R1: was a
  blocker).** A *server-principal* cred is autonomously decryptable by every delegate, so
  the power to write one must NOT be conferred by the bare `/v1` bearer alone — a single
  leaked thin-client bearer would otherwise mint server-owned secrets that every delegate
  silently trusts. Server-principal writes therefore require a **dedicated capability**
  (e.g. `vault:write:server`) granted separately from the transport bearer, and **every**
  write emits a **structured audit event** — `{caller_principal, transport, agent, cred,
  action, ts}` — to a dedicated audit sink (not an INFO log line). Where the caller has an
  attested identity (UDS `uid:`/webchat `webuser:`), prefer scoping the write to that
  principal over the server principal. Bulk/off-hours server-principal writes are
  alert-worthy.
- **D2b — transport binds to ATTESTED IDENTITY, not IP (R2: was IP-based).** WP-1 forwards
  plaintext API keys client→server. A server-principal write is **refused** unless the conn
  is `UDS_PEERCRED` or `WEBCHAT_TRUSTED` (the `attested_transport` enum already exists,
  parent D10). A `TCP_BEARER` conn — even from loopback — is **denied** for server-principal
  writes, because an IP/loopback check is bypassable via SSH-tunnel / localhost-forward. If a
  TCP thin client must provision creds, it does so over **TLS + the `vault:write:server`
  capability**, or via a local UDS relay; cleartext key forwarding over a `TCP_BEARER` conn is
  never accepted. (Refusal returns the explicit 4xx of D11, with a remediation hint.)
- **D2c — capability provisioning, bounded (R2: was undefined).** The `vault:write:server`
  capability is **operator-minted on a UDS connection** (`aimee vault capability grant`,
  `peer_uid`-attested) and stored server-side in a 0600 grants record, separate from the
  transport bearer; it is **revocable** (`… revoke`) and listed (`… list`). A leaked bearer
  alone cannot write a server cred. **Scope bound (R2):** this proposal introduces **exactly
  one** capability and one write path — a general capability/grant model (read, rotate,
  locked-override) is explicitly **out of scope / future**, so we do not grow an unbounded
  authz subsystem here. **Audit sink:** every server-principal write appends to the existing
  dedicated `audit.log` (append-only, 0600, not readable by the delegate-execution principal)
  a record `{caller_principal, attested_transport, capability_id, agent, cred,
  key_fingerprint(sha256, first 8 bytes — NEVER the key), action, ts}`; a `severity=high`
  tag on bulk/off-hours writes is grep-able for alerting.
- **D3 — explicit migration affordance.** Add `aimee vault set --server <agent> <cred>
  <secret>` (and/or `aimee agent key import`) that forwards to a server route calling
  `vault_service_set_server`, so existing keys migrate without re-adding agents.
- **D4 — ship the updated client to `.254`** (and any other thin clients). Confirm the
  client version delta does not regress the `uid:` UDS path.

### WP-2 — Codex (and codex-style OAuth) vault-write + refresh

R1 resolved the bespoke-vs-generic fork: **codex is generalized onto the existing
`oauth_tokens.c` refresh layer**, so D5–D8 below are one coherent path, not parallel ones.

- **D5 — codex device-setup writes the oauth_tokens schema.** On a successful device-code
  exchange the server writes, under `(VAULT_SERVER_PRINCIPAL, "codex", …)`: the
  `oauth_access_token`, `oauth_refresh_token`, and `oauth_expires_at` slots (the
  `oauth_tokens.c` schema), plus `VAULT_CODEX_ACCOUNT_CRED` (the ChatGPT-Account-ID, which
  is codex-specific and used to build the request header). No bespoke codex token slot.
- **D6 — refresh via the shared layer; "fallback" is operator-attended, not autonomous
  (R2).** Codex token use goes through `oauth_token_get()`, which refreshes when
  `oauth_expires_at` is past: POST the OpenAI token endpoint with `grant_type=refresh_token`,
  the stored `refresh_token`, and client_id `app_EMoamEEZ73f0CkXaXp7hrann` (OpenAI's published
  Codex CLI client_id — pin with a source link in the impl). On success re-vault the new
  access token + expiry (the refresh token rotates only if the response returns a new one).
  **Expiry handling, stated plainly:** access-token-expired → refresh. **refresh-token
  expired/revoked (refresh returns 400/401)** → the server **cannot** run a browser device
  flow, so it does **not** silently retry: it marks the codex cred `REAUTH_REQUIRED`, fails
  the delegate with an explicit "codex re-auth required" error, and the **operator** runs
  `aimee codex reauth` (the D7 remote device flow). Calling this a "fallback" in rev. 2 was
  misleading; it is a human-attended re-auth.
- **D6b — `oauth_tokens.c` is keyed by `(principal, service, cred)` (R2).** The service name
  (`codex`) is a **first-class key**, not a comment, so a second OAuth consumer cannot collide
  with codex. `VAULT_CODEX_ACCOUNT_CRED` remains the authoritative source for the
  ChatGPT-Account-ID request header; the `oauth_*` slots are authoritative for the token —
  the two never overlap.
- **D7 — setup against the remote.** Fix `agent setup` so the device-code exchange +
  vault-write run against the **configured remote** server, not a local one (closes G4).
- **D8 — lazy-migrate `codex-auth.json` → vault, ATOMICALLY (R2).** On read, if the vault has
  no codex token but a disk `codex-auth.json` exists: write **all** slots
  (`oauth_access_token`, `oauth_refresh_token`, `oauth_expires_at`, account-id) to the vault,
  then **verify by decrypt-roundtrip** that every slot reads back, and **only then** scrub the
  disk file. A partial write (some slots written, some not) **rolls back** (or leaves the disk
  copy intact) — never a scrubbed disk + an incomplete vault. Never delete the only copy.

### WP-3 — Migrate existing credentials into the vault (atomic, resumable, reversible)

- **D9 — explicit, idempotent, per-agent-verified import.** Import the live keys —
  the **discovered** agent set (R2: not a hard-coded list) — read `agents.json` +
  `agent-keys.json` at run time so the state machine converges on the operator's actual
  agents, not the `minimax/mistral/mimo-2.5/glm` snapshot named here — and codex (WP-2) into
  the server vault via an **explicit operator command** (`aimee agent key import` /
  `vault set --server`), not silent auto-import (R1 fork-d). Each agent is a tracked unit
  with a state in **`NOT_STARTED → IMPORTED → VERIFIED → LEGACY_SCRUBBED`**:
  - **Encrypted backup-before-scrub (R2):** take an **encrypted** vault snapshot before the
    run (the plaintext `agent-keys.json` is the existing risk, not a new backup file); nothing
    is scrubbed until a durable vault copy is confirmed.
  - **Vault-level verifier (R2: stronger than a smoke call):** after IMPORT, verify by a
    **decrypt-roundtrip directly from the vault slot** (bypassing the credential-resolution
    chain so a still-live `/v1/session/credentials` cannot emit a false VERIFIED), AND a
    health probe that authenticates. Only on both → VERIFIED, then scrub that agent's
    `agent-keys.json` entry → LEGACY_SCRUBBED. A failed verify leaves the agent legacy and is
    reported.
  - **Per-agent transition lock (R2):** a lock per `(agent)` so a thin client and the coord
    dispatcher cannot race the same agent's import; `--force` overwrite is itself audited.
  - **Idempotent + resumable:** re-running skips VERIFIED/LEGACY_SCRUBBED agents; a vault
    locked / interrupted run resumes from the recorded state.
  - **Migration-incomplete is observable (R2):** `vault status` (and a server startup log
    line) report any agent not yet `LEGACY_SCRUBBED`, so an un-run import does not silently
    stall on the legacy path forever.

### WP-4 — Retire the legacy paths (feature-flagged rollback, version-gated)

- **D10 — `vault_only` rollback flag, with an enumerated blast radius + dry-run + cutover
  gate (R2).** A server config flag `vault_only` governs whether legacy credential sources are
  accepted. `false` = vault-first with legacy fallback (resolution order: **vault → env-lease
  → agents.json `$ENV`**, made explicit per call, not just per agent); `true` = vault is the
  **sole** source. It is the rollback lever: flipping back to `false` restores legacy paths
  without redeploy.
  - **Blast radius (R2: must be enumerated, not assumed).** Flipping `vault_only=true` makes
    every credential-injection surface fail-closed for an un-migrated agent: the **in-model
    delegate tool**, **webchat** dispatch, the **coord dispatcher**, and **jobs** all resolve
    through `vault_service_inject_api_key` → these are the surfaces that 401 if their agent is
    not `LEGACY_SCRUBBED`. WP-4 ships a **`--dry-run`** that reports exactly which agents ×
    surfaces would fail **before** the flip.
  - **Cutover gate (R2: not open-ended).** `vault_only` flips to `true` only when the gate is
    met: **0 legacy-path (`/v1/session/credentials` / `codex-auth.json`) successes in
    `audit.log` for N days AND explicit operator sign-off** — not a vague "≥1 release." The
    flag then remains as a kill-switch.
- **D11 — retire the client `agent-keys.json` push** (`cli_agent_keys.c` +
  `/v1/session/credentials`). Per R1 fork-c: retire the **write** path; keep the
  `/v1/session/credentials` **read** path only as an **opt-in, audited** fallback for
  genuinely non-vaultable `$ENV`-ref agents, removed once no agent depends on it. Under
  `vault_only=true`, `POST /v1/session/credentials` from an old client returns an explicit
  **4xx with a clear error** (never silent accept-and-drop).
- **D12 — retire the `codex-auth.json` disk read** as a **migration-only** read for one
  release (D8), then delete. Vault becomes the sole codex source.
- **D13 — operational completeness (R1).** Before WP-4 flips `vault_only=true`: document and
  test a **`.server-master.key` rotation** procedure (re-wrap, not re-encrypt); define
  **`agent add --key` upsert/rotation semantics** on an already-vaulted agent (overwrite +
  audit, not silent duplicate); and verify the **fail-closed-on-locked** path with the
  legacy fallback removed.

## Non-goals

- Direct-TCP per-user (`uid:`) vault — stays future work (parent D17). Shared delegate keys
  belong to the **server principal** by design.
- No HSM/external KMS; the master key is the server-held `.server-master.key`.
- No webchat changes beyond the already-built `webuser:` path.
- No change to provider wire protocols or the agent loop.

## Tests

- **WP-1:** `agent add --key` over TCP lands the secret in the server vault (at-rest
  ciphertext only; `agents.json` holds no plaintext); a delegate authenticates from the
  vault with `agent-keys.json` **absent**; a UDS add resolves to the `uid:` principal.
- **WP-2:** codex setup against the remote stores token+account in the server vault; a codex
  delegate authenticates from the vault; an expired token auto-refreshes and re-vaults; an
  existing `codex-auth.json` lazily migrates then is scrubbed.
- **WP-3:** all four API delegates **and** codex authenticate purely from the vault on
  `.254` with `agent-keys.json` removed.
- **WP-4:** with `vault_only=true`, every delegate authenticates from the vault; a locked
  vault fails closed (no silent downgrade); `POST /v1/session/credentials` from an old client
  returns an explicit 4xx; flipping `vault_only=false` restores legacy fallback without
  redeploy; a grep of the at-rest store + `agents.json` finds no plaintext credential.
- **Negative/failure paths (R1):** refresh-token expired/revoked → "re-auth required" +
  operator `codex reauth` (not silent fail); `.server-master.key` corrupt/truncated → fail
  closed; vault locked **mid-migration** → import is idempotent + resumes; an old client +
  new server coexist during the transition window; a concurrent re-add vs an in-flight
  delegate run does not corrupt state; a server-principal write without the `vault:write:server`
  capability is rejected and audited.
- **R2 residual coverage:** a server-principal write over a `TCP_BEARER` conn (even loopback)
  is **refused** (attested-transport, not IP); a granted capability is revocable and a revoked
  one is denied; the audit line carries a **key fingerprint, never the key**; `vault_only
  --dry-run` lists every (agent × surface) that would 401; the per-agent verifier rejects a
  false-VERIFIED when `/v1/session/credentials` is still live (decrypt-roundtrip bypasses the
  resolution chain); a partial `codex-auth.json` migration rolls back (no scrubbed-disk +
  incomplete-vault); two concurrent imports of one agent are serialized by the lock.

## Forks — decided in R1

1. **Server-principal write authz (D2)** — the `/v1` bearer is **not** sufficient. Require a
   dedicated `vault:write:server` capability + per-write audit + a transport requirement
   (D2/D2b), scoped to an attested identity where one exists.
2. **Codex token layer (D5–D8)** — **generalize codex onto `oauth_tokens.c`** (one refreshing
   OAuth layer with lazy migration), not a bespoke codex path.
3. **`session_credentials` #186 (D11)** — retire the **write** path; keep the **read** path as
   an opt-in, audited `$ENV`-ref fallback only, removed once unused.
4. **Migration trigger (D9)** — **explicit operator import** with backup + per-agent verify,
   **not** silent auto-import (auto-import would push plaintext-at-rest keys onto the network
   from every workstation and create a self-service path on reinstall/new-user).

## Review history

- **R1** (rev. 1): security **BLOCKED**, reviewer **CONCERNS**; architect (mistral) lost to a
  429, engineer (glm) to a reasoning-model stall. → rev. 2 (D2 capability + audit, migration
  state machine + `vault_only`, codex onto `oauth_tokens.c`, forks decided).
- **R2** (rev. 2): security **CONCERNS**, engineer **BLOCKED**; architect + reviewer (mimo-2.5)
  lost — the larger doc **exceeded mimo-2.5's context window** (`caps=tools, min_context=6049`).
  → rev. 3 (capability provisioning/revocation + attested-transport-not-IP + audit fingerprint;
  blast-radius enumeration + dry-run + cutover gate + vault-level verifier + per-agent lock +
  discovery; codex device-flow-is-operator-attended + atomic migration + service-keyed schema).
- **Fleet limitation (real, affects the gate).** Across both rounds the **only** model that
  reliably reviews a doc this size is **minimax**: **glm** stalls on long reviews, **mistral**
  429s, **mimo-2.5** is context-capped on rev. 2+. So additional rounds yield more *minimax*
  opinions, not more *diversity*. A genuinely diverse panel needs the fleet work already
  proposed in [roundtable-panel-composition](./roundtable-panel-composition.md) +
  [curator-llm-backend](./curator-llm-backend.md). Given that, rev. 3 goes to the **USER
  proposal-approval gate** rather than spinning further single-model rounds.

## Evidence (premise pinning — re-flagged in R1 and R2)

Both rounds' sandboxed server-side reviewers flagged "server is v0.2.58 / no `.vault/`" because
they cannot curl `/v1/version` (bash blocked, `verify` 401s) and read a **stale** `server.log`
banner (`vv0.2.58`, dated 2026-06-13 — a *prior boot*; the append-only log survives redeploys).
The authoritative facts: `GET /v1/version` on the **running** process returns
`{"version":"v0.2.68","service":"aimee-server"}`; the on-disk binary
`/usr/local/bin/aimee-server` (built 2026-06-14 08:31) `--version` = `v0.2.68`; `aimee vault
list` over TCP returns the vault-specific "no attested local identity" error (vault code is
live). The absent `.vault/.server-master.key` is the **expected** pre-first-write state — it is
created by the first server-principal write this proposal introduces.
