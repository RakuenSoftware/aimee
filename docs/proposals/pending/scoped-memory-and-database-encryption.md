# Encrypt document and memory bodies; keep search queryable

**Status: pending proposal.** Scope agreed 2026-09-06. Runtime implementation has
not started.

Aimee's vault already provides encryption, key wrapping, and protected-use calls.
Document and memory writers still persist plaintext. The work is to connect those
paths while separating the authority to search from the authority to decrypt.

We will encrypt original documents and memory bodies with vault-managed scope
keys. Whole-database encryption will be optional and can run alongside payload
encryption or on its own. Existing installations keep their current behavior until
configured and migrated.

## Whole-database encryption covers the searchable data

Whole-database encryption protects stored indexes while allowing the running
database to search them. That covers the metadata protection we need.

| Data | Required protection |
|---|---|
| Original documents and memory bodies | Scoped payload encryption; database encryption when enabled |
| Metadata, embeddings, and full-text indexes | Database encryption when enabled; queryable while running |

Search derivatives can reveal content. We accept that exposure to someone with
full access to the running database and its unlock keys. Encrypted vector search
is outside this proposal. With database encryption disabled, metadata and indexes
have no encryption-at-rest guarantee from this feature.

Database-unlock authority remains separate from payload-key authority. Reading an
unlocked database should leave original bodies encrypted until the vault permits
a scoped operation. A compromised consumer can expose whatever it is authorized
to read. Repairing a compromised host or database service remains an environmental
responsibility; this feature cannot protect plaintext and keys already available
to the attacker.

## Five controls limit what one credential can expose

### 1. Search returns candidates; authorized reads decrypt bodies

Give search a separate service identity and database privileges limited to
metadata and search representations. Its results contain candidate IDs, versions,
scores, and permitted metadata. Its credentials confer no decryption permission.

A separate payload reader resolves selected candidates. Content-dependent ranking,
substring verification, and snippets run there. Ingestion receives its own bounded
authority to process plaintext and produce search derivatives.

Current [memory retrieval](../../../src/kb/db2_adapters/kb_service_backend_memory.c)
loads bodies into `memory_t` while ranking. Introduce candidate-only contracts
below that layer. Separate runtime credentials and service admission must enforce
the distinction through direct calls and relays. Shared privileged connections
would defeat it.

**Acceptance:** normal search works with only the search identity. That identity
cannot fetch plaintext, invoke decryption, or assume a payload-reader role. The
authorized recall path still returns the intended content.

### 2. The vault uses scope keys internally

Callers request an operation on a record for an authenticated actor, scope, and
purpose. The vault resolves the keys and returns the permitted result. Search,
delegates, and payload consumers receive no scope-key export operation.

Build on the existing [protected-use calls](../../../src/kb/kb_vault_protected_use.h)
and their plaintext cleanup. Process permissions and distinct service identities
must supply runtime isolation; a callback alone cannot. Follow the
[per-daemon vault bus design](vault-bus-only-access.md), preserving each daemon's
custody profile. Relaying a request confers no additional decryption authority.

**Acceptance:** caller-facing contracts return no scope keys. Unauthorized,
expired, or wrong-purpose requests release no plaintext, including through relays.
Audit the actor, record reference, scope, operation, and outcome. Exclude keys and
bodies from audit records.

### 3. Changing metadata cannot grant payload access

Bind record identity, owning workspace/project, purpose, format, and version to the
authenticated envelope. The vault checks ownership and the actor's authority before
releasing plaintext. A scope supplied by a database row or request identifies the
record; it supplies no proof of access.

A trusted authority must issue decryption grants outside ordinary content-row
write privileges. Define verification at the vault boundary, including any grants
or memberships stored in the database. Checking another freely editable row would
leave the same vulnerability.

Moves and sharing require authorized operations that update envelopes and search
versions consistently. Check current authorization state for freshness and
revocation: authenticating a version field alone cannot detect replay of an entire
old valid envelope.

**Acceptance:** row relabeling, envelope substitution, forged request scope, and
expired or revoked grants confer no access. Authorized moves and sharing remain
usable.

### 4. Protect retained body copies wherever Aimee writes them

The [memory writer](../../../src/modules/db2/c/memory_score_fields.c) and
[document writer](../../../src/modules/db2/c/kb_payload.c) bind plaintext into text
columns. Add encrypted formats to their writes, updates, and reads. Cover originals
in file/object storage, extracted text, chunks, memory units, quoted excerpts, and
persisted caches. Inspect summaries, facts, provenance, and rejection tombstones
for copied bodies or independently stored memories.

The [schema](../../../src/modules/db2/c/schema.sql) also stores
`memories_code_fts_text = key || ' ' || content` for trigram search. Remove that
complete body copy. Maintain token and vector indexes separately during authorized
ingestion, keeping their versions consistent through updates and deletion.
Replace direct `lower(d.content) LIKE ...` queries with candidate lookup and
substring verification in the payload reader. Search compatibility needs testing.

Backups and machine exports preserve encrypted envelopes. Intentional plaintext
exports require payload-read authority and an explicit output-protection policy.
The current [database export script](../../../deploy/container/aimee-kb-db-export.sh)
writes a logical dump to `/tmp/aimee-db2-export.dump`; database-file encryption
alone establishes no protection for that output. Include temporary files and logs
in the inventory of places Aimee may write bodies.

**Acceptance:** ingest distinctive bodies and inspect persisted outputs for
complete plaintext copies, allowing the agreed search derivatives. Exercise
successful, failed, and interrupted ingestion/export. Account for old plaintext
rows, free pages, WAL, snapshots, and backups during migration. Updating a row alone
cannot establish historical erasure.

### 5. Bound active keys and runtime privileges

Keep cached scope keys inside vault-owned memory and load only scopes needed for
active use. Enforce bounded lifetimes. Every operation checks actor, purpose,
expiry, and revocation even when its key is cached.

Extend the existing [key-cache lifecycle](../../../src/modules/vault/vault_kek_cache.h)
so lock, seal, revocation, and relevant rotation invalidate dependent entries.
Cleanse keys on expiry, shutdown, and fork. Define how in-flight operations finish
so a completed lock prevents new uses of stale authority. Choose cache capacity,
TTL, and batch limits before implementation and measure their cost.

The hardened profile already has [runtime-role assertions](../../../src/modules/db2/c/db2_hardening.c).
Extend them to search and payload roles wherever separated payload access is
enabled, including personal deployments. Check effective memberships as well as
direct privileges. Runtime identities cannot own protected tables, bypass row
security, perform migration DDL, or assume privileged roles. Keep migration
credentials outside runtime services. Connection scope settings provide an
additional filter; vault authorization needs its own verified grant.

**Acceptance:** test expiry, lock/revocation races, unrelated-project isolation,
and shutdown/fork cleanup. Reject startup with overprivileged or interchangeable
search/payload identities. Legacy installations report only the protection they
actually enforce.

## Each payload requires every applicable scope key

| Scope | Required content keys |
|---|---|
| Global | Global |
| Workspace | Global and the owning workspace |
| Project | Global, the owning workspace, and the project |

Use immutable scope IDs and an explicit owning workspace for each project. Map
existing memory scopes and workspace associations to that ownership. Adding an
association must not silently create another way to decrypt the record.

Encrypt each body once with a fresh random data key. Scope keys protect that small
key using [envelope encryption](https://docs.cloud.google.com/kms/docs/envelope-encryption).
The proposed format nests authenticated wraps under project, workspace, then global
keys. Decryption opens them in reverse. Finalize the format and cryptographic
review before implementation, including large-document handling.

Use independent random scope keys to enforce the all-keys condition. A parent key
that unwraps stored child keys would let global-key possession recover descendants.
Separate alternative wraps of one data key would allow any one key to decrypt it.
Neither construction supplies the required all-keys condition.

Keep the global content key distinct from the vault custody root. Storing every
scope key in one vault still gives an authority allowed to use all of them access
to every payload. The five controls above restrict that authority.

Wrapping-key rotation can rewrap data keys without rewriting bodies. Global
rotation may still visit every affected envelope. A compromised data key requires
payload re-encryption. Existing autonomous server credential wraps must provide no
bypass around the content-key requirements. Already disclosed plaintext cannot be
revoked.

## Database encryption needs a provider and a boot path

We found no whole-database encryption option in the inspected source and deployment
paths. Choose supported providers and configuration names before implementation.
For SQLite, evaluate SQLCipher's page encryption and driver/build compatibility.
For PostgreSQL, use a verified encrypted-storage or managed-database facility;
stock PostgreSQL has no equivalent application toggle. Its
[encryption options](https://www.postgresql.org/docs/17/encryption-options.html)
describe the available layers.

Report provider readiness per database. Enabling encryption with an unavailable
or unverifiable provider must produce a configuration/readiness error. Specify
coverage for indexes, WAL/journals, temporary files, replicas, and physical backups.
Logical dumps and external document files need their own protected paths.

The database must obtain its unlock key before opening its vault tables. Keep that
bootstrap path outside the database, using the existing file/KMS/TPM/PKCS#11 custody
architecture and daemon separation. Database encryption can use a distinct
deployment key. Enabling, disabling, rotating, and restoring encryption require
recoverable workflows; they cannot be instantaneous runtime switches.

## Cipher timings leave the retrieval cost unmeasured

A temporary local C benchmark on 2026-09-06 compiled the existing
[vault primitives](../../../src/modules/vault/vault_crypto.c) with `cc -O2` and
OpenSSL 3.5.6. Each size ran 20,000 iterations. Writes generated a fresh 32-byte
data key, wrapped it with AES-KW, and encrypted the body with AES-GCM and a fresh
nonce. Reads unwrapped and authenticated/decrypted the body; recovered bytes were
checked for equality.

| Body size | Envelope write | Envelope read |
|---|---:|---:|
| 1 KiB | 3.69 microseconds | 4.58 microseconds |
| 4 KiB | 3.86 microseconds | 4.95 microseconds |
| 16 KiB | 5.42 microseconds | 6.46 microseconds |
| 64 KiB | 11.44 microseconds | 12.37 microseconds |

These exploratory averages exclude database access, vault/bus calls, authorization,
serialization, external custody, and the additional scope layers. The temporary
harness was discarded; the measurement did not record CPU details or a source
commit. The figures establish no deployment performance guarantee.

The [existing envelope constants](../../../src/modules/vault/vault_crypto.h) add
68 bytes: a 40-byte wrapped key, 12-byte nonce, and 16-byte tag. Scope layers and
identity/version metadata add further overhead. Password derivation belongs at
unlock. Bounded vault-side caching or batches should avoid per-record remote
custody calls while preserving each operation's authorization checks.

Benchmark ingestion, insert/read/recall, rotation, and recovery on each supported
backend. Include realistic sizes and concurrency, warm/cold caches, and external
custody. Compare payload encryption, database encryption, and both enabled. Report
p50/p95/p99 latency, throughput, CPU, storage, and index-load cost. Measure search
changes separately from cipher cost. We have no overall percentage estimate yet.
[SQLCipher's vendor guidance](https://www.zetetic.net/sqlcipher/performance/)
describes overhead as low as 5–15%; it does not estimate Aimee's workload.

## Deliver authority separation before enabling the mode

1. **Settle the contracts.** Define ownership mapping, grant issuance/verification,
   envelope format, payload inventory, runtime identities, and storage providers.
2. **Establish protected use.** Add scope-key lifecycle and authenticated ownership.
   Provision service admission and database roles before granting runtime access.
3. **Convert storage and retrieval.** Introduce candidate-only search and authorized
   reads; convert ingestion, updates, retained copies, and exports together.
4. **Add database encryption.** Implement provider configuration, bootstrap, and
   readiness checks alongside verification of search/payload privileges.
5. **Migrate and prove recovery.** Supply resumable conversion and key-recovery
   procedures. Account for historical plaintext artifacts and interrupted work.
6. **Verify the complete paths.** Run the five acceptance checks, wrong/missing-key
   and tamper tests, scope moves, rotation/crash recovery, and provider-failure
   tests. Run the workload benchmarks before enabling separated payload access.
