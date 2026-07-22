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
`src/modules/config/config_fields.c:156`; `config` parses and projects those values.
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

`test_vault_service.c`, `test_vault_store.c`, `test_vault_seam.c`, capability, principal, bootstrap,
seal, rotation, PostgreSQL, KMS, PKCS#11, and TPM2 suites cover the layered contracts. Wrong principal,
locked/sealed state, corrupt ciphertext, missing entry, expired capability, or custody failure must
return typed failure and never expose partial plaintext or select a weaker backend silently.

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
