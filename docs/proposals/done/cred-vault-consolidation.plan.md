# Implementation plan: credential-vault consolidation

Companion to [cred-vault-consolidation.md](./cred-vault-consolidation.md) (rev. 3, USER-approved
2026-06-14, gate 1). Base branch: `testing` (the vault lives there; the docs branch does not).
Each packet is **independently shippable as its own PR**, built + tested + reviewed + merged
before the next (per the autonomous-proposal-loop). Smallest/safest, highest-live-value first.

## Sequencing

### P1 — Codex/OAuth into the vault + setup-against-remote (WP-2)  ← FIRST (fixes live 401)
- Generalize codex onto `oauth_tokens.c`, keyed by **(principal, service, cred)** (D6b).
- Codex device-setup writes `oauth_access_token`/`oauth_refresh_token`/`oauth_expires_at` +
  `VAULT_CODEX_ACCOUNT_CRED` under the **server principal** (D5).
- `oauth_token_get()` refresh for codex; revoked/expired refresh → `REAUTH_REQUIRED` +
  `aimee codex reauth` operator command — **no autonomous browser flow** (D6).
- Fix `agent setup` to target the **configured remote** server, not local (D7).
- Atomic `codex-auth.json` → vault lazy migration: write-all → verify-roundtrip → scrub (D8).
- **Value:** closes the live codex 401; mostly server-side + a small client setup-routing fix.
- Files (testing): `src/server/oauth_tokens.c`, `src/server/server_agent.c` (codex setup),
  `src/server/agent_config.c` (`agent_read_codex_oauth_token` use-path),
  `src/server/delegate_credential_retry.c`, `src/cmd_agent_setup.c` (remote routing),
  new `aimee codex reauth` command. Tests: `src/tests/test_oauth_*`, a codex-vault test.

### P2 — Capability core + client key-forward (WP-1, D2/D2b/D2c)
- `vault:write:server` capability: UDS-minted, server-side 0600 grants store, revocable;
  `aimee vault capability grant|revoke|list` (D2c).
- Server-principal writes gated on **attested transport** (UDS_PEERCRED / WEBCHAT_TRUSTED),
  refused for TCP_BEARER even on loopback (D2b); audit line with **key fingerprint** (D2c).
- Client `agent add --key <literal>` forwards the secret to `/v1/agent/add` (stop writing
  `agent-keys.json`); `aimee vault set --server` import affordance (D1/D3).
- Files: `src/server/server_vault.c`, `server_agent.c`, `vault_service.c`/`vault_store.c`
  (audit + capability gate), `src/cli_agent_keys.c`/`cmd_agent.c` (client forward), new
  `cmd_vault` capability subcmds. Tests: capability grant/revoke/deny; transport refusal.

### P3 — Migration state machine (WP-3, D9)
- `aimee agent key import`: discovery (read agents.json + agent-keys.json), per-agent state
  `NOT_STARTED→IMPORTED→VERIFIED→LEGACY_SCRUBBED`, encrypted backup-before-scrub, **vault-level
  decrypt-roundtrip verifier** + auth probe, per-agent lock, idempotent/resumable,
  `vault status` migration-incomplete surfacing.
- Files: new migration command + state store; `cmd_agent.c`, `cmd_vault`. Tests: false-VERIFIED
  rejection, partial-run resume, concurrent-import lock.

### P4 — `vault_only` + legacy retirement (WP-4, D10–D13)
- `vault_only` server flag (default false) + `--dry-run` (enumerate agent×surface 401s) +
  cutover gate (0 legacy reads N days + sign-off) (D10).
- Retire `cli_agent_keys.c` push; keep `/v1/session/credentials` read as opt-in audited
  `$ENV` fallback (D11). Retire `codex-auth.json` read (migration-only one release) (D12).
- `.server-master.key` rotation procedure/cmd; `agent add --key` upsert semantics (D13).
- Files: `config*`, `server_compute.c`/`delegate_credential_retry.c` (resolution order +
  vault_only gate), `cli_agent_keys.c`, `agent_config.c`. Tests: vault_only flip, old-client
  4xx, rollback, fail-closed.

## Cross-cutting

- **Build/test discipline:** `make -j1` on this host (parallel-LTO flakes); never merge red
  (`gh pr checks`); local verify-local ≠ CI green. Security-critical crypto/authz paths get
  unit tests + the `OPENSSL_cleanse`/at-rest-no-plaintext greps the parent proposal pins.
- **Plan gate (per [[aimee-dev-workflow]]):** this plan is roundtable-reviewed before P1 code.
  Fleet caveat: only minimax reviews a doc this size reliably — same constraint as the proposal
  rounds (see proposal §Review history).
- **Per-packet loop:** implement → build green → roundtable review → fix to APPROVE → PR →
  USER pass/fail → squash-merge → next packet.
