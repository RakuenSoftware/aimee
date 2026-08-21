# vault module

## Purpose and non-goals

`vault` is required core and owns principal-scoped secret custody, encryption, controlled retrieval
and injection, sealing, rotation, and custody-provider binding. It does not own provider login flows,
Git or forge protocols, optional OIDC governance, tool execution, workspace selection, or the policy
that decides whether a caller may request a credential.

## Public contracts

Canonical implementation lives in `src/modules/vault`: the server-wrap retrieval path begins at
`src/modules/vault/vault_service.c:60`, unlock at line 150, and set/get at lines 265/294;
`vault_store_*` is the storage facade, `vault_server_key` manages sealing, `vault_principal` binds
attested identities, and `vault_capability` limits delegated access. Direct calls to internal vault
implementations outside these facades are boundary violations rather than alternate public APIs.

The descriptor declares this module's twelve sources, thirteen module-root headers, eleven direct
tests, and this document; it sets `ownership_complete: true`. All thirteen headers are declared
as `private_headers` because they live at the module root rather than under
`src/modules/vault/include/aimee/vault/`, the layout the header-layout checker treats as private;
`vault_internal.h` is the backend-seam header and has no paired source. The four custody backends
(`vault_custody_kms.c`, `vault_custody_mock.c`, `vault_custody_pkcs11.c`, `vault_custody_tpm2.c`) are
always compiled by Make: `WITH_TPM2` and `WITH_PKCS11` toggle a `-D` flag that switches the tpm2 and
pkcs11 sources between a real implementation and a fail-closed stub rather than omitting them.
`vault_hwm.c` has no external includer but is live: `vault_custody_kms.c` calls
`vault_hwm_attest_verify` in its high-water-mark attestation path. CMake compiles the six sources the
thin `aimee` client reaches (`vault_capability.c`, `vault_crypto.c`, `vault_kek_cache.c`,
`vault_principal.c`, `vault_service.c`, `vault_store.c`) and omits the four custody backends,
`vault_hwm.c`, and `vault_server_key.c`, the same intentional thin-client boundary recorded for
gateway, learning, and workspace. `docs/validation/core-modularization-slice-46.md` records the declaration audit and
`docs/validation/core-modularization-slice-47.md` the completeness audit; the two were split so the
latch reviews declarations merged on their own first. Adding a new module-local source or module-root
header without declaring it now fails CI on `rule=ownership-complete`.

## Dependencies and consumers

- `config`: supplies selected custody provider and provider-specific effective settings.
- `execution-policy`: authorizes credential creation, lookup, injection, rotation, and administration.
- `module-runtime`: supplies required lifecycle and readiness contracts for credential custody.

Consumers include [delegates](delegates.md), `tools`, `git`, `workspace`, provider clients, and server
vault handlers. Optional governance consumes principal and custody contracts for organizational
identity; its absence cannot remove local principal isolation or secret protection.

## Providers and readiness

`vault_store_backend_t` supports the local encrypted store and PostgreSQL binding, while
`vault_custody_provider_t` has file/software, KMS, PKCS#11, and TPM2 implementations. Readiness
requires exactly one selected usable storage/custody path and a valid seal state; configured hardware
that cannot attest or unseal must fail concretely rather than downgrade to an undeclared provider.

## Configuration and activation

- `runtime_toggle.supported`: `false`; credential custody is required while the storage backend and custody provider are selectable.

### Config touchpoint

The module interprets `vault.custody` and `vault.tpm2.*` as registered at
the external config module's field metadata; `config` parses and projects those values.
Environment/bootstrap credential import and per-provider names are input surfaces, not ownership of
the secret lifecycle. Provider fields with no compiled and selected consumer must remain hidden.

## Surfaces

Surfaces include `aimee vault`, server vault routes, unlock/lock/rekey/seal operations, credential
presence checks, provider bootstrap, and scoped injection into delegates or Git operations. Lists
and health output expose references and state only; protocol clients and GUI forms must never receive
stored secret values merely to display whether a credential exists.

## Data and migrations

Encrypted records carry principal, agent/provider, credential name, wrapped data key, ciphertext,
salt, and format/version metadata in file or PostgreSQL stores. `vault_store_rekey` and server-wrap
migration preserve principal ownership and atomic recoverability. Plaintext, KEKs, unlock passwords,
and transient injection buffers are never migration payloads.

## Security and privacy

Secret references may cross into `git`, `tools`, and [delegates](delegates.md); raw values
cross through bounded buffers in `vault_service_get*`, `vault_service_inject_api_key`, Git credential
environment construction, and provider request setup. Those consumers must zero/free or contain the
value, avoid argv/log persistence, and remain policy- and principal-scoped; static evidence cannot
prove every dynamic sink, so full runtime non-leakage remains a hypothesis, unverified.

## Supported journeys

An attested principal unlocks or uses an already authorized server wrap; execution-policy approves a
credential request; `vault_service_get*` resolves the named secret through the selected custody and
store providers; the consumer injects it into one bounded operation; and audit records only reference,
principal, decision, and outcome metadata before transient plaintext is discarded.

## Tests and failure behavior

The descriptor's eleven direct tests are `test_vault_capability.c`, `test_vault_crypto.c`,
`test_vault_kek_cache.c`, `test_vault_kms.c`, `test_vault_master_rotate.c`, `test_vault_principal.c`,
`test_vault_seam.c`, `test_vault_server_key.c`, `test_vault_service.c`, `test_vault_store.c`, and
`test_vault_tpm2.c`. The last backs the `WITH_TPM2`-gated `p7-tpm2-harness` and is the only test that
links and exercises the real `vault_custody_tpm2.c`, so it is claimed despite the gate. Several
`test_vault_*` files exercise other modules and are not claimed: `test_vault_audit.c` pins
`vault_audit_server_write` in `src/server/server_vault.c` (server), `test_vault_seal.c` and
`test_vault_tpm2_stub.c` drive the `kb/kb_vault_policy.c` seal barrier (KB), `test_vault_pg.c` links
`kb/kb_main.c` (KB integration), and `test_vault_bootstrap.c` drives `server_vault_bootstrap.c`
(server). `test_vault_custody_pkcs11.c` is an orphan that no build target compiles; it is left
undeclared and flagged for a later cleanup rather than claimed as coverage. `vault_hwm.c`,
`vault_custody_mock.c`, and `vault_custody_pkcs11.c` have no declared direct test; they are exercised
transitively (the high-water-mark path through `test_vault_kms.c`, the mock custody through the
service and store suites). Wrong principal, locked/sealed state, corrupt ciphertext, missing entry,
expired capability, or custody failure must return typed failure and never expose partial plaintext or
select a weaker backend silently.

## Operational diagnostics

Report selected provider, readiness, sealed/locked state, principal class, credential reference,
rotation generation, cache count, and redacted status from `vault_status_str`. Logs and metrics must
exclude ciphertext when unnecessary and always exclude plaintext, passwords, KEKs, tokens, private
keys, injected environments, and replayable attestation material such as signatures or nonces.
Cross-module raw-secret evidence is collected in the [Slice 16 validation record](../validation/core-modularization-slice-16.md).

## Compatibility

`vault_service_*`, principal syntax, status values, encrypted record versions, provider seams, and
seal/rekey recovery are compatibility contracts. Store changes must support explicit migration and
rollback; legacy secret locations may be imported once but cannot remain an independently readable
fallback that bypasses principal, policy, or custody checks.

## Extension and removal

New store or custody providers implement the existing internal vtables and prove failure, rotation,
and non-downgrade behavior. Forge OAuth belongs to `git`, while federated OIDC/SSO belongs to optional
governance; neither should be absorbed because it handles credentials. Core vault cannot be removed,
and wrapper paths used only by their own tests are candidates for a later liveness audit.
