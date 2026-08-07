# Vault: reachable only by core, over the event bus

- **State:** PENDING — operator ruling 2026-08-07.

## The invariant

Nothing may reach the credential vault except **core**, and core reaches it **over the event bus**.

A thin client, a delegate, or any module that needs a credential operation sends a request to core
over the bus; core is the bus, so routing is core's to permit or deny. Relay hops are fine. Direct
access is not.

## Why the bus is not where the violation lives today

`git`, `delegates`, and the `kb/*` binary do not route anything. They **link** the vault and call it
as ordinary C functions, so core is not in the path at all and has nothing to permit or deny. There
is no `vault` module under `server-go/modules/` — it is the one component with no bus counterpart
(17 modules there: `git`, `memory`, `tools`, `workspace`, `routing`, `delegates`, …).

Extraction is therefore what *creates* the chokepoint. Enforcement follows from it; it cannot
precede it.

## Direct callers to sever

Non-core modules calling `vault_service_*`:

| File | Calls |
|---|---|
| `src/modules/git/git_forge_vault.c` | 4 |
| `src/modules/git/git_host_cred.c` | 4 |
| `src/modules/git/git_oauth_github.c` | 4 |
| `src/modules/delegates/delegate_credential_retry.c` | 3 |
| `src/modules/git/git_oauth_device.c` | 2 |

Callers reaching **below** the service layer, straight to `vault_store_*`:

`src/db2/vault_pg.c`, `src/kb/kb_main.c`, `src/kb/kb_vault_rewrap.c`,
`src/kb/kb_mgmt_status_provision_main.c`, `src/modules/git/git_host_cred.c`,
`src/server/server_vault.c`, `src/server/server_vault_bootstrap.c`,
`src/server/server_vault_agent_migration.c`.

The `kb/*` entries are a separate binary: today the KB process opens the vault store directly rather
than asking core for anything. How KB obtains credentials once it cannot open the store is the
sharpest open question in this migration, and it should be answered before any code moves.

Core call sites to convert (direct calls become bus requests): `server_vault.c`, `pki.c`,
`oauth_tokens.c`, `server_agent.c`, `agent_config.c`, `server_vault_bootstrap.c`,
`server_vault_agent_migration.c`, `server_http_routes_git.c`, `server_cli_oauth.c`,
`vault_audit_bridge.c`.

## Deliverables

- Extract a `vault` bus module: event-kind range, principal ref, module descriptor, lock pin,
  mirroring how an existing process module is declared.
- Convert core's call sites to bus requests. `/v1/vault/*` stays as the thin-client entry point —
  the client sends a command to core, core asks the vault. The HTTP surface is not the violation.
- Route only core to the vault's event kinds, so a module addressing it directly is refused rather
  than merely discouraged. This is the same "core tap" the event-bus enforcement residual describes.
- Sever the module and KB direct linkage **last**, so nothing is half-cut: while a caller still
  links the store, the chokepoint is advisory.
- Answer KB credential provisioning explicitly rather than by omission.

## Sequencing

Extract → convert core → enforce routing → revoke direct linkage. Reversing the last two leaves a
window where callers are cut off before the replacement path carries them.

## Completion evidence

A fixture must **fail** when any non-core component reaches the vault — by direct call or by
addressing its event kinds — and the credential-dependent paths (git forge auth, delegate
credential retry, KB provisioning, agent key resolution) must each be proven over the bus, not
merely compiled.

## Not in scope

Authorization of the `/v1/vault/*` routes themselves. That is a separate, already-actioned concern:
`vault.list` was gated only by `CAP_DELEGATE`, which `CAPS_AUTHENTICATED` includes, so any unrevoked
client cert enumerated every server credential name. Fixed alongside this proposal, independently of
where the vault runs.
