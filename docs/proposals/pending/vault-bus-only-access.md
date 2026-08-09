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

## KB credential provisioning — answered

This was the open question. The answer is that **there is no single vault to reach**, so the
extraction produces one vault module per daemon bus, not one custodian shared across daemons.

Three facts settle it.

**The daemons do not share a bus.** `obs_bus_configure_daemon_module_runtime` derives both endpoints
from the daemon's own name — socket `<config>/<daemon>-module-bus.sock`, policy
`<config>/modules.d/<daemon>`. `server_main.c` passes `"server"`; `kb_main.c` passes `"kb"`. A module
attached to one is unaddressable from the other. Any design where KB asks the server's vault is a new
cross-daemon credential path, with a bootstrap ordering dependency KB does not have today.

**The two vaults hold different data in different stores.** `vault` is declared
`placements: ["server", "kb"]`, and each daemon links its own instance. The server keeps the built-in
jsonfile backend; `kb_main.c` rebinds to `vault_pg_backend` after `db2_init`. Server credentials are
not in KB's store and KB's are not in the server's.

**That split is the delivered design, not drift.** P10 (`../done/tiered-llm-p10-shared-vault-core.md`)
extracted one vault core "linked into both binaries — instantiated as two profiles": the server
profile selects file custody with local storage and a single user; the kb profile selects
external-anchor custody, Postgres storage, multi-tenant, sealed and WORM-audited. The profile decides
custody and storage; the core is shared. Collapsing the two into one bus module would undo P10's
assurance boundary, not tighten it.

So: **each daemon serves a vault module on its own bus, instantiated with that daemon's profile.**
Topology is unchanged — the same two custodies over the same two stores — and the chokepoint lands on
each bus, which is the only place routing can enforce it. KB obtains credentials from the module on
the KB bus, exactly the credentials it obtains today.

### The runtime callers convert; the offline one does not have to

KB's runtime vault callers all live inside the kb daemon, which hosts a bus: `kb_main.c`,
`enroll.c`, `kb/http/kb_http_identity_login.c`, `kb_vault_rewrap.c`. These become bus requests.

`kb_mgmt_status_provision_main.c` looks like a counterexample and is not one. It is a separate
program — an "offline, owner-only bootstrap tool ... deliberately absent from `all`, install targets,
runtime images, and ordinary KB linkage", and `test_build_integrity.sh` fails the build if its
symbols appear in the shipped `aimee-kb`. It runs with no daemon and therefore no bus, before the
system it provisions exists. It keeps direct store access, and the invariant is unaffected: the
invariant governs the running system, and this tool is by construction not part of one.

That distinction belongs in the enforcement fixture. A check that simply forbids linking
`vault_store_*` would fail this tool for doing the one thing it exists to do, so the rule is
"no direct access **from a component that ships in a runtime image**", which is the boundary
build-integrity already draws.

## Why `vault` may leave the C core when `execution-policy` and `audit` may not

The capability contract (`../done/aimee-core-capability-contract.md`) places `vault`,
`execution-policy` and `audit` in the C communication core as the trust kernel, "because the bus
authorizes and records every inter-module event through them and the safety boundary must not depend
on a module the bus is trying to reach". Taken at face value that forbids this proposal. It does not,
because the stated reason is not true of `vault`.

Nothing under `src/core/event_bus/` references the vault — not the runtime, not admission, not
routing. Attach admission (`bus_runtime.c`) compares the peer's uid and resolved executable path
against a `.grant` manifest on disk; both come from the kernel, neither from a credential. The bus
authorizes and records through execution-policy and audit. It never reaches the vault, so moving the
vault behind the bus cannot create the circularity the contract is guarding against.

The contract anticipates this: "Wherever a future amendment might move them, their contracts and
reference implementations stay required in every core profile." This proposal is that amendment, for
`vault` only. `execution-policy` and `audit` stay in the C core, where the argument does hold.

Concretely that means `CORE` in `scripts/validate_module_process_contracts.py` loses `vault`, the
contract entry gains `runtime`, `principal_ref` and `stages`, and the capability contract is amended
to record why the kernel is now two components rather than three. The amendment is a prerequisite of
the extraction, not a consequence of it — the validator refuses a core component that carries process
fields, so the two land together or neither does.

## Deliverables

- Extract a `vault` bus module: event-kind range, principal ref, module descriptor, lock pin,
  mirroring how an existing process module is declared. One instance per daemon bus, each
  instantiated with that daemon's P10 profile — see the section above.
- Convert core's call sites to bus requests. `/v1/vault/*` stays as the thin-client entry point —
  the client sends a command to core, core asks the vault. The HTTP surface is not the violation.
- Route only core to the vault's event kinds, so a module addressing it directly is refused rather
  than merely discouraged. This is the same "core tap" the event-bus enforcement residual describes.
- Sever the module and KB direct linkage **last**, so nothing is half-cut: while a caller still
  links the store, the chokepoint is advisory.
- ~~Answer KB credential provisioning explicitly rather than by omission.~~ Answered above.

## Sequencing

Extract → convert core → enforce routing → revoke direct linkage. Reversing the last two leaves a
window where callers are cut off before the replacement path carries them.

## Completion evidence

A fixture must **fail** when any non-core component that ships in a runtime image reaches the vault
— by direct call or by addressing its event kinds; the offline provisioner is excluded on the
boundary build-integrity already draws — and the credential-dependent paths (git forge auth, delegate
credential retry, KB provisioning, agent key resolution) must each be proven over the bus, not
merely compiled.

## Not in scope

Authorization of the `/v1/vault/*` routes themselves. That is a separate, already-actioned concern:
`vault.list` was gated only by `CAP_DELEGATE`, which `CAPS_AUTHENTICATED` includes, so any unrevoked
client cert enumerated every server credential name. Fixed alongside this proposal, independently of
where the vault runs.
