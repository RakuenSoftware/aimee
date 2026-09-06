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
Vault owns all keys, including the drive keys, and unlocks the drive through its
existing principals and protected-use operations. TPM is not required.
The default installation lets Aimee deploy PostgreSQL and provision encrypted
storage, with automatic key custody and unlock. Linux uses LUKS-backed storage.
Storage encryption can run alongside payload encryption or on its own; disabling
it requires explicit configuration. Existing installations migrate automatically
when first started with the updated deployment, retaining recoverable source data
until verification and cutover complete.

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
Use the existing AES-GCM primitive for variable-length authenticated wrapping:
encrypt the data key under the project key, that envelope under the workspace
key, and that envelope under the global key. Omit inapplicable scope layers.
Each layer has a fresh nonce and binds canonical record identity, owning scope,
field purpose, payload revision, and its layer/key revision as authenticated data.
Use length-prefixed fields and explicit format/domain tags. Decryption opens the
layers in reverse using the expected context from the authorized operation.
The existing fixed-size AES-KW helper accepts a 32-byte key; it cannot directly
wrap a larger nested envelope. Keep this new envelope format versioned separately.

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

Use `pgp_sym_encrypt_bytea` and `pgp_sym_decrypt_bytea` for payload bytes. Vault
generates a fresh random 32-byte data key for each field revision; pass its canonical
hex encoding as the PGP password. Pin AES-256, integrity checking enabled,
compression disabled, and `s2k-mode=3,s2k-count=65536`. PGP derives its cipher key
from that supplied value; it does not consume the bytes as a raw AES-GCM key.

The encrypted message contains a versioned, length-prefixed context followed by
the original payload bytes. Its context records the same record identity, owning
scope, field purpose, and payload revision authenticated by the Vault wraps.
Vault verifies those wraps against the authorized operation before supplying the
data key. The database read operation decrypts the message and checks its embedded
context against that operation before exposing the body. Caller input and mutable
row labels cannot override the expected context. A field update creates a new
data key and envelope; a wrapping-key rotation preserves the payload revision.

This combines Vault's authenticated scope wraps with PostgreSQL's PGP integrity
checks. Test ciphertext-only and whole-envelope swaps between records, fields,
scopes, and revisions; test each missing or wrong scope key. Keep cryptographic
review of the serialized format in implementation acceptance. PostgreSQL's raw
encryption functions supply no integrity check and are excluded from this path.

### Search projections preserve lexical and substring results

Keep the existing `tsvector` values, parser configuration, positions, and ranking
expressions for lexical search. Convert projections generated from body columns
into explicitly maintained values. Authorized PostgreSQL writes create the
encrypted payload and its search projections in one transaction. The current
[lexical queries](../../../src/modules/db2/c/memory_query.c) can rank those
projections without decrypting bodies. Return candidate IDs through the search
contract and resolve bodies through the authorized read operation.

For substring search, persist a distinct set of overlapping three-character
fragments for each searched field, including punctuation and whitespace. Store
the set as `text[]` with PostgreSQL's
[GIN array index](https://www.postgresql.org/docs/current/gin.html#GIN-BUILTIN-OPCLASSES).
For deterministic `lower(content) LIKE pattern` comparisons, extract fragments
from the same PostgreSQL-lowered content. Extract required query fragments from
literal runs in the actual bound pattern, respecting its wildcard and escape
rules. A candidate must contain every required fragment (`@>`). For example,
`%crypt%` requires `cry`, `ryp`, and `ypt`; every exact match contains all three.
Fragment membership may admit false positives, which the payload reader rejects.

Preserve each call site's current pattern construction. The
[PDF search](../../../src/modules/db2/c/kb_payload.c) escapes user `%`, `_`, and
backslash; several memory queries retain wildcard semantics. Union candidates
for OR branches such as key, content, and use cases. Ordinary word-token matching
cannot replace substring matching. `show_trgm(query)` also cannot supply the
required substring fragments directly because it adds word-boundary padding.

PostgreSQL decrypts authorized candidates and evaluates the original `LIKE`,
`ILIKE`, equality, and ranking expressions against their plaintext. For patterns
with no three-character literal run, negated predicates, or collation/case rules
without a proven conservative fragment filter, scan the metadata-filtered,
authorized rows. Preserve the original SQL semantics in that fallback. The
[trigram compatibility rewriter](../../../src/modules/db2/c/db_postgres.c) currently
uses `ILIKE` and `similarity`; replace that path with explicit candidate and
authorized-verification operations, retaining its exact scoring expression.

Apply limits after exact verification. If ordering depends on decrypted content
or similarity, evaluate every candidate needed to establish that ordering before
selecting the result page. With metadata-only ordering, process candidates in
that order until the page is full or the candidate set is exhausted. Batch size
controls memory use; it must never become an undisclosed result cutoff. Preserve
the existing tie behavior and use a consistent snapshot for a batched query.

Bind projections to the payload revision and normalization version. Writes,
deletes, and migration update them atomically; include rows with missing or stale
projections in the authorized fallback until rebuilt. Fragment sets are readable
search derivatives covered by the agreed storage-encryption boundary.

Verify result IDs, ordering, and scores against the existing plaintext queries
for partial words, punctuation, escaped wildcards, wildcard patterns, Unicode,
short and empty inputs, OR branches, updates, deletion, and multi-batch results.
These searches are feasible with encrypted payloads. Selective fragment filters
reduce decryption work; short or broad patterns can still require a full authorized
scan. No constant-latency claim follows from this design.

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

### Installation creates encrypted storage automatically

Make Aimee-managed PostgreSQL the standard setup path. Choosing it includes the
database container, encrypted storage, key enrollment, and restart integration.
Users need no separate disk-encryption setup or routine unlock step. Bringing an
existing database remains an advanced option with provider-specific readiness
checks. Define equivalent storage providers for other supported hosts before
claiming the same automatic deployment support there.

Replace the current PostgreSQL container definition with one that supports the
column-encryption integration and optional encrypted storage. Use the same image
for encrypted and unencrypted deployments. This requires no second PostgreSQL
container or separate encryption sidecar.

The default managed Linux installation creates a dedicated LUKS2 encrypted disk
image in Aimee's data directory and formats the filesystem inside it.
[Cryptsetup supports file-backed disk images](https://man7.org/linux/man-pages/man8/cryptsetup.8.html),
so installation can provision storage without repartitioning an existing drive.
Operators can supply a dedicated device or an existing encrypted mount instead.

| User action | Managed behavior |
|---|---|
| Install Aimee | Provision encrypted storage, enroll automatic key custody, and start the database |
| Restart Aimee or reboot | Recover the storage key, unlock and verify the mount, then start PostgreSQL |
| Use documents, memories, or search | Apply scoped access through the shared module; require no storage passphrase |
| Start the updated 0.4.0 deployment | Detect the existing layout, migrate automatically, verify it, and switch the database mount |

Setup installs the host integration with the required system privileges. The
PostgreSQL container keeps ordinary database privileges. Allocate and grow the
disk image within a configured storage budget; define capacity checks and
interrupted-growth recovery before implementation. Report encryption and storage
readiness in status, with an actionable error if provisioning or unlock fails.
Unsupported hosts require a supported storage provider or an explicit opt-out;
setup must never silently fall back to plaintext storage.

The host unlocks and mounts the filesystem before starting the database container.
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

Vault owns the storage keys and unlocks the drive through its existing principals
and key-management operations. Integrate its authorized use with libcryptsetup,
or supply key material through a private pipe to `cryptsetup`; avoid command-line
secret values and persistent plaintext key files. The
[LUKS unlock interface](https://man7.org/linux/man-pages/man8/cryptsetup-open.8.html)
accepts supplied key material without TPM. Setup and restart perform this operation
automatically before mounting storage and starting PostgreSQL. Keep drive and
payload keys distinct within Vault. Test this path on a host without TPM.

We found no whole-database encryption option in the inspected deployment paths.
Choose supported PostgreSQL storage or managed-service encryption providers and
configuration names before implementation. Stock PostgreSQL has no built-in
whole-database encryption toggle.

Report provider readiness per database. Enabling encryption with an unavailable
or unverifiable provider must produce a configuration/readiness error. Specify
coverage for data files, indexes, WAL, temporary files, replicas, and physical backups.
Logical dumps and external document files need their own protected paths.

Deployment startup orders the existing Vault service, drive unlock and mount,
then PostgreSQL startup. Storage provisioning and key operations use the existing
Vault integration. Enabling, disabling, rotating, and restoring encryption use
managed workflows; they cannot be instantaneous runtime switches.

## Automatic migration preserves 0.4.0 installations

Keep the product version at 0.4.0. Track schema and storage migration revisions
independently so startup can detect completed work without relying on the product
version. Existing users receive the migration through the normal deployment
update, with no separate migration command or required configuration rewrite.

For managed PostgreSQL, the deployment coordinator provisions encrypted storage
and custody, pauses writers, and migrates a consistent source into the new layout.
The shared database module converts payloads and selected metadata, preserves
record and scope identities, and updates search representations. Verify content,
permissions, and search behavior before switching connections and resuming writes.
Report migration progress and any startup delay.

Persist migration checkpoints and serialize competing startup attempts. Restart
must resume safely or recognize completed work. Keep the source recoverable until
cutover succeeds; after new writes reach the destination, recovery must preserve
those writes. Include retained plaintext migration artifacts in the cleanup policy.
Check storage capacity, custody availability, and required privileges before
changing the active installation.

Existing external databases use the same automatic schema migration through
their configured connections. Physical storage encryption requires support from
their hosting provider; preserve that deployment contract and report its coverage.

The 0.4.0 release criterion is compatibility: test supported existing installations,
fresh installs, repeat startup, interrupted migration, and recovery. A rollout that
requires manual conversion or leaves existing installations needing repair does
not meet this criterion. Resolve that incompatibility before shipping under 0.4.0;
a breaking migration would require reconsidering the release as 0.5.0.

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
identity/version metadata add further overhead. These constants describe the
existing AES-GCM envelope. The selected PGP payload format also performs S2K
derivation per message and has different overhead. Bounded vault-side caching or
batches avoid repeated custody work while preserving each operation's checks.

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
