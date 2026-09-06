# Encrypt document and memory bodies; keep search queryable

**Status: pending proposal.** Scope agreed 2026-09-06. Runtime implementation has
not started.

Aimee's vault already provides encryption, key wrapping, and protected-use calls.
Document and memory writers still persist plaintext. The work is to connect those
paths while separating the authority to search from the authority to decrypt.

Server and KB use the same database module. Implement the storage and query
contracts there, with caller permissions defining who can search or decrypt.
Historical source paths cited below identify conversion work; they do not establish
separate database-module ownership.

PostgreSQL will encrypt original documents, memory bodies, and selected metadata
columns. The vault will retain scope keys and authorize record-key use.
Whole-database storage encryption will be optional and can run alongside payload
encryption or on its own. Existing installations keep their current behavior until
configured and migrated.

## Whole-database encryption covers the searchable data

Whole-database encryption protects stored indexes while allowing the running
database to search them. That covers the metadata protection we need.

| Data | Required protection |
|---|---|
| Original documents and memory bodies | Scoped payload encryption; database encryption when enabled |
| Metadata | Database encryption when enabled; selective column encryption where the query contract supports it |
| Embeddings and full-text indexes | Database encryption when enabled; queryable while running |

Search derivatives can reveal content. We accept that exposure to someone with
full access to the running database and its unlock keys. Encrypted vector search
is outside this proposal. With database encryption disabled, metadata fields
without column encryption and the readable indexes have no encryption-at-rest
guarantee from this feature.

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
scores, and permitted metadata. Its credentials confer no document or memory-body
decryption permission. Access to an encrypted metadata field requires its own
authorized operation and confers no additional access to bodies.

A separately authorized payload-read operation resolves selected candidates.
Content-dependent ranking, substring verification, and snippets run there. The
shared database module serves both operations under distinct caller permissions
and database roles. Ingestion receives bounded authority to process plaintext and
produce search derivatives.

Current [memory retrieval](../../../src/kb/db2_adapters/kb_service_backend_memory.c)
loads bodies into `memory_t` while ranking. Introduce candidate-only contracts
below that layer. Separate runtime credentials and service admission must enforce
the distinction through direct calls and relays. Shared privileged connections
would defeat it.

**Acceptance:** normal search works with only the search identity. That identity
cannot fetch plaintext bodies, invoke payload decryption, or assume a payload-reader role. The
authorized recall path still returns the intended content.

### 2. The vault uses scope keys internally

Callers request an operation on a record for an authenticated actor, scope, and
purpose. The vault retains scope keys and authorizes the protected operation.
Search, delegates, and payload consumers receive no scope-key export operation.
The trusted PostgreSQL execution path receives a per-record data key for the
authorized operation. Define its lifetime and prevent
persistence in SQL definitions, logs, or pooled connection state.

Build on the existing [protected-use calls](../../../src/kb/kb_vault_protected_use.h)
and their plaintext cleanup. Process permissions and distinct caller identities
enforce admission to the shared module and vault; a callback alone cannot supply
process isolation. The module and PostgreSQL remain trusted during authorized
decryption. Follow the
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

Key selection follows the record's scope. Values in the same PostgreSQL column
can use different record keys for different workspaces or projects. `pgcrypto`
uses the key supplied for each operation; vault authorization enforces which
scope keys must participate. Assigning one project key alone would not enforce
the global, workspace, and project requirement.

Fields with different reader permissions use separate data keys and authenticated
field purposes. A metadata-read grant must not release a key that also decrypts
the document or memory body.

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

## The shared database module owns the PostgreSQL integration

Use one implementation for server and KB. Configure storage encryption, column
encryption, migrations, and readiness through the shared database module. Separate
search, payload, and migration privileges by operation and caller. Preserve
deployment-specific connection configuration without introducing a server/KB
implementation split.

PostgreSQL offers two relevant approaches in its
[encryption options](https://www.postgresql.org/docs/current/encryption-options.html):

| Approach | Search behavior | Protection |
|---|---|---|
| Data partition encryption | Existing queries and indexes continue to work | Filesystem or block-device encryption covers stored data and indexes |
| Specific-column encryption with `pgcrypto` | Queries explicitly decrypt values before using them | Selected column values persist as ciphertext; indexes need separate consideration |

Here, “data partition” means a storage partition. PostgreSQL table partitioning
does not itself encrypt data. The storage provider unlocks pages for normal
database execution.

Use [`pgcrypto`](https://www.postgresql.org/docs/current/pgcrypto.html) for column
encryption and decryption inside PostgreSQL. The vault manages independent scope
keys and unwraps record keys after authorization; the shared database module
supplies a record key only to the permitted database operation. Use protected
connections and prevent parameter logging from retaining keys or plaintext.

Preserve the all-scope-key condition and authenticated ownership. The PGP format
differs from the current AES-GCM envelope; finalize its format and ownership
verification before implementation. PGP message integrity alone does not bind a
ciphertext to its owning record. PostgreSQL's raw encryption functions lack
integrity protection and cannot replace the authenticated envelope on their own.

### Encrypting a vector column changes indexed search

An encrypted vector can be decrypted, cast back to `vector`, and used in a distance
calculation. Without a usable index over readable vectors, exact nearest-neighbor
search must decrypt and compare every row remaining after metadata filters.
`LIMIT` bounds the returned results; it does not bound that work.

[pgvector's indexes](https://github.com/pgvector/pgvector#indexing) operate on vector
representations. They cannot apply their distance operators directly to `pgcrypto`
ciphertext. A valid [expression index](https://www.postgresql.org/docs/current/indexes-expressional.html)
over decrypted values stores the computed values in the index. Encrypting the
source column alone would leave that derived representation outside its protection.
Index definitions must contain no decryption keys.

The current design retains queryable vector and token indexes under optional
storage encryption, with encrypted document and memory bodies. Encrypting the
vector column too remains an alternative to benchmark: compare exact decryption
scans, metadata-filtered scans, and the existing indexed retrieval. Column
encryption alone cannot promise both encrypted persisted vector indexes and
unchanged indexed search.

### Metadata fields can use column encryption independently

The vector-index limitation does not prevent encrypting other columns. Choose
fields by the queries they serve, and measure the resulting plans.

| Metadata use | Column-encryption approach | Query consequence |
|---|---|---|
| Display after selecting a record by ID | Decrypt the selected field during an authorized read | Existing ID lookup remains indexed |
| Exact-match lookup | Use a separate keyed lookup digest where justified | Equality remains indexable; repeated values remain observable |
| Range, sorting, grouping, or substring queries | Decrypt a bounded candidate set, or retain a permitted searchable representation | Decryption costs grow with candidates; retained indexes need storage protection |

An ordinary index on randomized ciphertext cannot perform the original plaintext
lookup. For keyed lookup digests, define field normalization and separate,
purpose-specific key use; verify matches against the decrypted value. An index
over decrypted expressions persists its computed values and therefore needs its
own at-rest coverage.

Keep metadata access separate from document and memory-body access in the shared
module. Add selected metadata fields, their permitted readers, and their query
requirements to the inventory before enabling column encryption. This option
does not require encrypting vectors or replacing their indexes.

### Docker can mount encrypted host storage

Replace the current PostgreSQL container definition with one that supports the
column-encryption integration and optional encrypted storage. Use the same image
for encrypted and unencrypted deployments. This requires no second PostgreSQL
container or separate encryption sidecar.

Offer an optional Linux deployment profile backed by a LUKS/dm-crypt device.
The host unlocks and mounts its filesystem before starting the database container.
Docker exposes the mounted directory through a bind mount or a configured
[volume](https://docs.docker.com/engine/storage/volumes/). PostgreSQL reads normal
pages; the host encrypts disk writes, including vector and token index files.

Use a dedicated encrypted data volume for Aimee's persistent storage. Encrypting
the filesystem backing Docker's entire data directory is also possible. Verify
the actual storage paths: Docker's
[`data-root`](https://docs.docker.com/engine/daemon/#daemon-data-directory)
does not relocate a separate containerd image store or external bind mounts.
Creating a named volume alone supplies no encryption.

Startup must verify the expected encrypted mount before initializing or opening
the database. A missing mount must fail readiness instead of creating plaintext
data in the underlying directory. Keep WAL, tablespaces, database temporary files,
and retained document files on covered storage; account separately for exports,
host temporary files, logs, and backups written elsewhere.

The storage key can use vault custody if that vault can unlock before this volume
is mounted. Otherwise bootstrap requires an independent custody path or operator
unlock. Keep the storage key separate from payload scope keys. Test locked boot,
missing mounts, restart, migration, and restore before enabling this profile.

We found no whole-database encryption option in the inspected deployment paths.
Choose supported PostgreSQL storage or managed-service encryption providers and
configuration names before implementation. Stock PostgreSQL has no built-in
whole-database encryption toggle.

Report provider readiness per database. Enabling encryption with an unavailable
or unverifiable provider must produce a configuration/readiness error. Specify
coverage for data files, indexes, WAL, temporary files, replicas, and physical backups.
Logical dumps and external document files need their own protected paths.

The selected provider must unlock storage before PostgreSQL opens its data files.
That boot path must work without reading vault tables inside the protected database.
Use the existing external-custody architecture where applicable and define the
provider-specific bootstrap integration. Storage keys remain distinct from payload
scope keys. Enabling, disabling, rotating, and restoring encryption require
recoverable workflows for each database; they cannot be instantaneous runtime switches.

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
PostgreSQL deployment. Include realistic sizes and concurrency, warm/cold caches,
and external custody. Compare payload encryption, database encryption, and both enabled. Report
p50/p95/p99 latency, throughput, CPU, storage, and index-load cost. Measure search
changes separately from cipher cost, including `pgcrypto` decryption scans and
indexed candidate selection. The AES-GCM timings above do not measure `pgcrypto`.
We have no overall percentage estimate yet.

## Deliver authority separation before enabling the mode

1. **Settle the contracts.** Define ownership mapping, grant issuance/verification,
   envelope format, payload and metadata inventory, runtime identities, and storage providers.
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
