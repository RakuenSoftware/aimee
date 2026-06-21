# Automatic delegate-vault provisioning at aimee-server standup

- **State:** **DONE / shipped.** Core (WP-A/B) landed in **PR #410**
  (`server_vault_bootstrap`, env scrub, audit, idempotence, `test_vault_bootstrap`);
  WP-C container surface is in tree (`combined-entrypoint.sh` + `server-entrypoint.sh`
  secrets-env docs, `runuser` env passthrough, the `compose.combined.yaml` /
  `compose.server.yaml` secret examples, and the SmoothNAS plugin field). Acceptance
  criteria 1–4 met and unit-verified; criterion 5 (re-provision the live `.254`
  server) is deployment-time validation. WP-D (remote provisioning over native TLS)
  is an explicit optional follow-on, tracked by
  [native-tls-thin-client-backends.md](../pending/native-tls-thin-client-backends.md).
- **Scope:** deterministic / server bootstrap + credential vault. Not an
  intelligence-surface proposal (no Architecture Charter role).
- **Author:** JBailes, 2026-06-17.
- **Origin:** surfaced while trying to run a roundtable against the `.254`
  server (v0.2.85): every delegate returned `HTTP 401 Unauthorized` because the
  server's vault held **no delegate credentials**, and the remote client-push
  path does not populate it. The validated keys exist on the client; the server
  just never received them at standup.

## Problem

A freshly stood-up `aimee-server` (Docker combined/split image, the SmoothNAS
plugin, or a bare `docker run`) comes up with an **empty delegate credential
vault**. Delegates resolve credentials **vault-first**
(`src/headers/delegate_credential_retry.h`: "vault-FIRST for `vault_principal`"),
so with an empty vault every `aimee delegate` / roundtable call fails
`provider '<x>' (HTTP 401): authentication failed`.

Today the only ways to populate the vault are **manual and co-located**:

- `aimee vault set <agent> <name> <secret>` / `aimee agent add --key`, which call
  `handle_vault_set` and require an **attested local identity** — they refuse the
  remote TCP transport (`vault: this connection has no attested local identity`).
- So an operator must shell into the server host/container and run the vault
  commands by hand after every fresh deploy.

This is the open tail of the cred-vault consolidation (creds → server vault):
the **sealing/storage mechanism shipped** (`handle_vault_set_server`, the
server-master-key dual-access wrap), but **nothing provisions the vault at
standup**, so the directive ("a new server should just work") is unmet. The
container entrypoints already seed `aimee.yaml` and ship `agents.json`
(definitions only — no secrets), but there is no equivalent step for secrets.

## Goal

**Standing up a new `aimee-server` provisions its delegate vault automatically**,
from a declared secret source, with no manual `vault set` and no client push, so
delegates (and therefore roundtables) work on first boot. Idempotent, secret-safe
(no plaintext left on disk), and a no-op when the vault is already populated.

## Design

At server startup, after the vault is unsealed under the server master key
(`.server-master.key` dual-access wrap) and **before** the delegate pool serves
traffic, run a **vault bootstrap pass** that seals any operator-supplied delegate
secrets into the per-server vault via the existing **server-principal** write path
(`handle_vault_set_server` / its internal callee), keyed by the server
`vault_principal`.

### Secret source (operator-declared, never baked)

Resolve secrets at boot from, in precedence order:

1. **A mounted secrets file** — `AIMEE_DELEGATE_SECRETS_FILE` (default e.g.
   `/run/secrets/aimee-delegates.json`), a `{ "<agent>": "<key>", … }` map. This
   is the Docker/compose/SmoothNAS-native path (Docker secret or bind-mounted
   file), mirroring how `agents.json` already supplies definitions.
2. **Environment variables** — `AIMEE_DELEGATE_KEY_<AGENT>` (e.g.
   `AIMEE_DELEGATE_KEY_MISTRAL`), for quick `docker run` / env-only deploys.

For each `(agent, secret)` whose agent exists in `agents.json` and which is **not
already present** in the vault, seal it under the server principal. Then **scrub
the source** from the process environment after ingestion (the file stays
operator-owned/read-only; we never copy plaintext into `$AIMEE_HOME`).

### Where it hooks

- **Server boot** (`src/server/…` startup, after vault unseal, before the compute
  pool accepts delegate work): the bootstrap pass is in-process so it uses the
  server principal directly — no attested-client requirement, no RPC.
- **Container entrypoints** (`deploy/container/combined-entrypoint.sh`,
  `server-entrypoint.sh`): document/pass `AIMEE_DELEGATE_SECRETS_FILE`; the
  compose files and the SmoothNAS plugin config gain an optional secrets mount.
  No secret is ever baked into an image.

### Idempotence & precedence

- Vault-present wins: an already-vaulted `(agent, cred)` is left untouched (the
  bootstrap never overwrites a rotated/operator-set value unless
  `AIMEE_DELEGATE_SECRETS_OVERWRITE=1`).
- Bootstrap runs every boot but is a no-op once the vault is populated, so
  restarts and recreates are cheap and safe.

## Out of scope

- The vault sealing/storage mechanism itself (already shipped:
  `handle_vault_set_server`, server-master-key wrap).
- OAuth/subscription delegates that mint per-request creds (codex OAuth) — those
  follow their own path; this is for static API-key delegates.
- Multi-user/`uid:`/`webuser:` principal vaults — this provisions the **server
  principal** vault that backgrounded delegates and roundtables use.
- Key rotation UX (covered by existing `vault set` / `vault rekey`).

## Risks

- **Secret hygiene.** The whole point is to avoid plaintext-at-rest: the secrets
  file must stay operator-owned and read-only, env vars must be scrubbed
  post-ingestion, and nothing may be written into `$AIMEE_HOME` or logs. An audit
  line (`vault_audit_server_write`) should record *that* a cred was provisioned,
  never its value.
- **Wrong-source clobber.** Default must be non-destructive (never overwrite an
  existing vaulted cred) so a deploy can't silently revert a rotated key.
- **Boot ordering.** Must run after vault unseal and before the delegate pool
  serves, or the first roundtable after a cold start still 401s.
- **Definition/secret mismatch.** A secret for an agent missing from
  `agents.json` should warn (not fail boot).

## Acceptance criteria

1. A fresh combined-image stack started with a delegate secrets file/env comes up
   with a populated vault; `aimee delegate review … --via mistral` succeeds with
   **zero** manual `vault set`.
2. No plaintext secret is written to `$AIMEE_HOME`, the image, or logs; env
   secrets are scrubbed after ingestion; an audit line records the provisioning
   event without the value.
3. Re-running/recreating the container does not overwrite an existing vaulted
   cred (unless the explicit overwrite flag is set) and is otherwise a no-op.
4. A secret naming an agent absent from `agents.json` warns and is skipped; boot
   still succeeds.
5. The remote roundtable path that 401'd against `.254` (v0.2.85) succeeds once
   the server is re-provisioned this way — verified end to end.
