# KB-optional deployment through module composition

Status: implemented in the 0.4.2 candidate; release validation and publication are in
progress. See [the unified deployment validation](../../validation/release-0.4.2-unified-2026-09-07.md)
for exact tested images, regression results, remaining gates and operational limits.

Memory is already a process module available in both Server and KB compositions. Extend
that implementation. Database ownership and authenticated access grants determine which
store a module can address. Do not create separate Server and KB memory implementations.

Embedding and synthesis services remain coupled to the composition that uses them. Either
composition can provision local model containers or configure external targets. Their
credentials must come from the owning composition's authorized identity services, without
requiring a KB to bootstrap a personal deployment. Choosing a remote KB must not hide or
replace the user's local embedding and synthesis configuration.

The local composition must support first boot, personal-memory storage, vector persistence,
recall and optional synthesis with no KB container, endpoint, database or identity provisioner.
A configured KB adds access to shared knowledge. It never becomes fallback storage for
personal records or their derived data, and equal numeric IDs do not confer cross-store access.

Acceptance requires fresh-volume and upgrade tests, actual local model startup and inference,
module/grant denial tests, dependency outage and recovery, and canary checks proving that
personal data stays within the local composition. Shared deployments must retain their
existing success-path contracts and independent model configuration.

## Instance identity and common infrastructure

Operator decisions, 2026-09-06:

- Ship one application image. First boot durably establishes exactly one identity:
  Server or KB. Later configuration changes and restarts cannot change that identity.
- Implement Server and KB as Go composition modules using the existing standardized
  modules. Core owns event-bus admission and must reject simultaneous or conflicting roles.
- Both identities have Vault and PostgreSQL with the same storage architecture.
  There is no KB-specific data volume requirement. Each instance retains its own
  identity, Vault state and database records.
- The standard single-user Compose deployment installs Server, local embedding and
  optionally synthesis. KB installation leaves both the base Compose and the web wizard.
  Connecting to an existing KB remains independently configurable. The `kb_mode`
  connection preference is not the immutable instance identity.

## PostgreSQL encryption and Vault-only key custody

The standardized PostgreSQL container must use LUKS encryption for both identities.
Vault is the sole persistent custodian of the LUKS unlocking key. Do not add TPM,
PKCS#11, KMS, systemd enrollment, a recovery key store, plaintext key files, Compose
secrets, environment variables or command-line key arguments to this storage path.
The LUKS header necessarily contains cryptographically protected keyslots; it must
never contain a plaintext copy of the Vault credential.

Core must make the instance's Vault available before the PostgreSQL storage unlock.
The current KB runtime's PostgreSQL-backed Vault binding cannot be a prerequisite
for retrieving the key that unlocks that same PostgreSQL database. Reuse the
standard Vault's database-independent persistence and authorized resource boundary;
do not create a second credential store for bootstrap. Vault state must remain
outside the PostgreSQL encrypted filesystem it unlocks.

The PostgreSQL module owns storage lifecycle and requests its credential through
core's authorized Vault resource path. Hand off the unlock credential only through
bounded, transient memory/pipe transport to cryptsetup. Exclude it from logs, crash
dumps, swap and durable diagnostics. Limit storage administration privileges to the
container bootstrap; the database process must run as the unprivileged PostgreSQL
user. Failure to obtain the matching Vault key or mount the encrypted filesystem
must prevent PostgreSQL startup, including initdb, WAL and temporary-file writes.
No plaintext fallback is permitted.

Never format an existing unrecognized device or replace a missing key for an
existing encrypted volume. Initialization must distinguish an empty, newly allocated
volume from an interrupted initialization and an existing deployment. Migration of
existing plaintext clusters requires a verified encrypted copy and preserves the
original until the migration is validated. Restores must preserve the association
between Vault state and the corresponding encrypted volume.

Additional release evidence must cover both immutable identities: fresh encrypted
initialization, successful restart and actual PostgreSQL read/write recovery, Vault
unavailability and wrong/missing keys, concurrent and interrupted initialization,
plaintext-volume migration, corrupt headers, unauthorized key requests, and absence
of plaintext keys or data canaries in container metadata and persistent storage.
A passing mocked lifecycle test alone does not establish container encryption.
