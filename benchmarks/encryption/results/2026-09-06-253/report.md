# Indexed candidates keep PostgreSQL payload decryption small

**Correction, 2026-09-06:** The user identified that the original figures could
not represent the intended Optane device. I placed those fixtures on `rpool`,
a mirror of two Patriot P220 SATA SSDs. Its 5,567 MiB/s ordinary read result came
from a cached backing-file path. Presenting the corresponding 52% throughput
drop as an Optane storage penalty was wrong. The original results remain below;
the corrected Optane measurements show an 8.9% single-worker sequential-read
reduction and a 4.1% increase in a cold PostgreSQL scan's median latency with
default LUKS settings. Neither establishes an application-wide percentage.

On .253, an indexed substring search over 100,000 encrypted 4 KiB bodies returned
20 matches in about 4.5 ms. Adding short fragments and a candidate probe reduced
a two-character no-match lookup from 9.5 seconds to 0.07 ms. Full body-similarity
ranking still took 40.4 seconds, against 30.5 seconds for plaintext bodies on
ordinary storage.

These measurements ran on 2026-09-06 in real Docker containers directly on
192.168.1.253. Both databases contained the same synthetic corpus. One used
ordinary ext4 storage, the other ext4 inside LUKS2. Each database contained
plaintext and encrypted tables, giving four configurations for each query.

## Full scans dominate the payload cost

Median query latency in milliseconds, with 100,000 eligible rows except the
project-filtered case:

| Query | Ordinary, plain | Ordinary, encrypted | LUKS, plain | LUKS, encrypted |
|---|---:|---:|---:|---:|
| Lexical, 20 matches | 0.23 | 1.87 | 0.25 | 1.82 |
| Substring, 20 matches | 1.19 | 4.48 | 1.24 | 4.54 |
| Broad substring, ordered first page | 0.48 | 2.43 | 0.48 | 2.48 |
| Common two-character term, first page | 0.42 | 1.99 | 0.43 | 1.99 |
| Absent two-character term, 100,000 rows | 1,501.46 | 9,527.81 | 1,505.15 | 9,687.00 |
| Absent two-character term, 1,000 project rows | 15.33 | 100.39 | 15.12 | 95.47 |
| Body-similarity ranking, 100,000 rows | 30,504.31 | 40,374.67 | 33,105.32 | 43,291.42 |

The original broad plaintext query chose a trigram bitmap scan that visited all
100,000 rows before sorting its first page. It took about 1.33 seconds. The table
uses the [ordered-page comparison](ordered-page.json), which places an evaluation
barrier on both payload representations and verifies matching results. Its plans
visit 20 bodies. That plan choice accounts for the apparent speedup in the
original query; encryption adds work to the comparable first-page path.

The broad similarity query scores every candidate. On ordinary storage, payload
encryption raised its median from 30.50 to 40.37 seconds. On LUKS, it rose from
33.11 to 43.29 seconds. The plans wrote about 136 MiB of temporary data for
plaintext and 393 MiB for encrypted bodies. The decrypted CTE values also lose
the source table's compressed representation. This case includes scoring,
decryption, and temporary-file work.

An absent short term scaled from 917 ms at 10,000 encrypted rows to 9,528 ms at
100,000 on ordinary storage. A project filter reduced the latter to 100 ms for
1,000 rows. Disabling the encrypted fragment filter for the rare term likewise
forced a scan. These results support candidate selection and scope filtering as
requirements for retrieval. They supply no fixed percentage for payload
encryption across workloads. All timings and plans remain in
[search.json](search.json).

## Short fragments and a candidate probe remove the avoidable scan

The three-character projection cannot filter a two-character literal. Adding
one- and two-character fragments reduced the encrypted `%qq%` no-match lookup
to 238 ms on ordinary storage and 241 ms on LUKS. Its ordered join still scanned
the projection. A separate candidate probe made PostgreSQL use its GIN lookup.

| Encrypted no-match lookup, 100,000 rows | Ordinary | LUKS |
|---|---:|---:|
| Three-character projection, payload scan | 9,527.81 ms | 9,687.00 ms |
| Short fragments, direct ordered join | 238.33 ms | 241.27 ms |
| Short fragments, candidate probe | 0.07 ms | 0.06 ms |

The probe fetches at most 513 IDs without result ordering. Up to 512 IDs form a
complete candidate list; an empty list returns immediately. If a 513th ID arrives,
retrieval uses the full metadata-ordered path. Both statements use one repeatable-
read transaction. The threshold chooses a query strategy and supplies no result
cutoff. The tests exercised empty, complete-small-list, and overflow paths.

With the probe, a common two-character term returned its first 20 encrypted
bodies in 3.11 ms on ordinary storage and 3.19 ms on LUKS. A less common numeric
pair exercised both retrieval strategies and preserved the plaintext results.
The [direct join](short-fragments.json) and [probe results](short-probe.json)
retain their plans and all samples.

The separate short-projection table added 229.65 MiB, including a 33.79 MiB index.
The original encrypted tables and indexes occupied 1,069.43 MiB, compared with
416.43 MiB for plaintext. That is 2.57 times the plaintext footprint, or 3.12 times
with the separate short projection. These totals include TOAST storage and
indexes, and exclude WAL and transient sort files. Combining all fragment lengths
in one production array can change the incremental storage cost.

The proposal now includes short fragments and the probe. Negation and unsupported
case/collation rules retain the authorized fallback. Broad content-dependent
ranking still has to evaluate its candidates. The optimization preserves the
measured deterministic `lower(body) LIKE pattern` contract.

## The original reads measured a cached SATA-backed path

The original test wrote 512 MiB files and immediately read them, using 1 MiB
sequential operations and 4,000 random 4 KiB reads per trial. The table reports the median of three trials.

| Operation | Ordinary | LUKS |
|---|---:|---:|
| Sequential write, including fsync | 52.51 MiB/s | 57.43 MiB/s |
| Sequential read | 5,567.41 MiB/s | 2,700.58 MiB/s |
| Random 4 KiB read latency | 8.08 microseconds | 16.57 microseconds |

`O_DIRECT` bypassed the inner ext4 page cache. The loop devices had backing-file
direct I/O disabled, and ZFS data caching remained enabled. The 5,567 MiB/s result
exceeds this SATA mirror's physical throughput and exposes the cached path.
The 52% difference is invalid as an Optane or physical-device encryption penalty. Write trials varied from 43 to
128 MiB/s on ordinary storage and 43 to 66 MiB/s on LUKS, so these samples do not
isolate a write penalty. [Raw I/O results](direct-io.json) retain that variation.

Warm selective SQL searches changed little between storage variants. The broad
ranking query had higher medians on LUKS and wrote substantial temporary data;
its samples also varied with the shared host. A single storage-encryption
percentage would hide those different paths.

## Optane reads reach the device with AES acceleration active

The corrected images reside on `optane/aimee-encryption-bench`, backed by the
Intel SSDPE21D960GA at `/dev/nvme0n1`. The
[storage inventory](verified-storage.txt) records both pools and their devices.
Only this isolated dataset has `primarycache=metadata`, `secondarycache=none`,
and `direct=always`; compression and deduplication remain disabled. No global
host cache was flushed. The [dataset properties](optane-storage.txt) preserve
the configuration.

Enabling `losetup --direct-io=on` failed with `EINVAL` on this backing filesystem.
The completed test therefore retains buffered loop devices, recorded as `DIO=0`.
ZFS's `direct=always` requests direct handling of aligned I/O, while
`primarycache=metadata` excludes file data from ARC. See the
[OpenZFS property definitions](https://openzfs.github.io/openzfs-docs/man/master/7/zfsprops.7.html).
The test records physical Optane read counters around every sample and rejects
samples with fewer device bytes than 95% of the requested bytes. Sequential
samples read about 1.00–1.05 device bytes per requested byte. These counters
cover the shared device, so other services can contribute traffic.

Opening the LUKS mapping raised the reference count of `xts-aes-vaes-avx2` from
one to two while the other XTS drivers' counts stayed unchanged. Its module is
`aesni_intel`, and its self-test passed. This identifies the accelerated driver
selected by the mapping; the [raw diagnostic](optane-diagnostic.json) records
both states. The separate memory-only [cipher benchmark](aes-xts-benchmark.txt)
measured 8,612 MiB/s AES-XTS decryption. That rate is not a disk measurement.
Docker also exposes `aes` and `vaes` in the PostgreSQL containers' CPU flags.

The read-only diagnostic uses identical 512 MiB fixture files, one outstanding
request per worker, 1.5 seconds per sample, and three shuffled repetitions.
These are medians on the shared host:

| Read workload | Ordinary | LUKS default | LUKS without workqueues | LUKS same CPU |
|---|---:|---:|---:|---:|
| Sequential 1 MiB, one worker, MiB/s | 1,894.25 | 1,725.33 | 1,452.42 | 1,482.84 |
| Sequential 1 MiB, eight workers, MiB/s | 2,018.53 | 2,075.47 | 1,936.34 | 1,995.12 |
| Random 4 KiB, one worker, mean microseconds | 102.00 | 105.07 | 107.75 | 107.16 |
| Random 4 KiB, eight workers, mean microseconds | 758.79 | 763.56 | 755.76 | 749.56 |

Default LUKS reduced single-worker sequential throughput by 8.9%. Eight-worker
medians reversed order by 2.8%; three short trials do not establish an encryption
speedup. Workqueue bypass reduced sequential throughput further on this stack,
so these results support retaining the default scheduling. Random reads fetched
about 32 device bytes per requested byte, consistent with the outer 128 KiB ZFS
records. Their latencies describe the layered image path and cannot stand in for
raw Optane 4 KiB latency. [All samples](optane-diagnostic.json) retain the byte
counts, load averages, driver state, and scheduling flags. The separate
[cached rpool diagnostic](luks-diagnostic.json) is retained for comparison.

## PostgreSQL's cold scan costs 4.1% more in this Optane fixture

The same two Docker containers were restarted against the copied Optane images.
Each cold run stopped its container and unmounted/remounted its inner ext4
filesystem before restarting PostgreSQL. This discards PostgreSQL buffers and
the inner filesystem cache; the outer dataset excludes data caching throughout.
No VM or replacement database engine was used.

The query scans 100,000 encrypted rows and hashes every ciphertext, forcing
payload reads without adding PGP decryption. All runs returned the expected
count and aggregate. Medians from three shuffled repetitions:

| Ciphertext scan | Ordinary | LUKS default | LUKS without workqueues |
|---|---:|---:|---:|
| Cold, milliseconds | 1,002.22 | 1,043.05 | 1,082.10 |
| Immediate warm repeat, milliseconds | 788.01 | 759.75 | 768.22 |

Cold samples recorded 1,030–1,037 MiB of physical Optane reads; warm repeats
recorded 0–1.31 MiB. Default LUKS added 4.1% to the cold median. Individual cold
samples ranged from 969–1,146 ms ordinary and 1,040–1,113 ms encrypted, so the
median difference is descriptive and does not establish a precise overhead
bound. The [PostgreSQL results](optane-postgres.json) include SQL, plans, device
counters, CPU flags, and the unchanged four-CPU container quotas. This scan
isolates one storage-sensitive database operation. The original search matrix
was not rerun on Optane, and Optane writes remain unmeasured. The measured costs
are acceptable for the proposed default at-rest encryption; further tuning is
not a delivery gate.

## Write and scope-wrap timings have narrower boundaries

A committed 100-row batch took 98.43 ms for plaintext and 85.03 ms for encrypted
bodies on ordinary storage; the LUKS medians were 94.68 and 93.29 ms. Both schemas
maintained their indexes and lexical projections. The schemas do different index
work, and fragment extraction occurs before timing. These are database batch
measurements with prepared inputs. They supply no end-to-end ingestion speedup
or encryption-only write percentage. [Write results](write.json) contain all five
samples; [initial-load timings](load.json) record the one-time bulk loads.

Four-client bursts issued three requests per client. Indexed encrypted substring
queries had median latencies of 6.35 ms on ordinary storage and 6.67 ms on LUKS.
The original unindexed no-match scans took 10.41 and 10.31 seconds per request.
Those twelve-request bursts illustrate contention at the configured limits;
they do not establish sustained service capacity. See
[concurrency results](concurrent.json).

Using the repository's Vault primitives with OpenSSL 3.5.6, generating a fresh
32-byte data key and nesting three authenticated scope wraps took 2.37
microseconds per operation. Unwrapping all three took 1.33 microseconds. These
are medians of five runs of 100,000 operations. Each wrong scope key, changed
associated data, and tampered ciphertext was rejected. The
[native results](scope-wrap.json) measure cipher work; Vault service calls and
authorization remain outside them.

## The original query matrix used Docker on rpool

The host has an Intel Core i7-14700K, 28 logical CPUs, AES instructions, and about
125 GiB RAM. Other services continued running. Docker Engine 28.5.2 ran through
a dedicated socket and data directory. Each PostgreSQL container had a four-CPU
quota, 6 GiB memory limit, no network interface, and a Unix socket for the client.

Both containers used PostgreSQL 18.6 from the same pinned `postgres:18` image.
Their settings included 512 MiB shared buffers, 64 MiB work memory, UTF-8 with
`C.UTF-8`, JIT disabled, and parallel query workers disabled. Checksums, `fsync`,
synchronous commit, and full-page writes were enabled. Autovacuum was disabled
for the controlled fixture; tables were explicitly analyzed. Exact settings and
the image digest are in [search.json](search.json) and
[postgres-image.txt](postgres-image.txt).

The original two 12 GiB disk images occupied `rpool/aimee-encryption-bench`,
backed by the mirrored Patriot SATA SSDs, with outer compression,
deduplication, and encryption disabled. Both inner filesystems used ext4 with `noatime` and
completed initialization. LUKS used AES-XTS with a 512-bit combined key and
512-byte sectors. PostgreSQL data, WAL, and temporary files used the mounted
filesystems. Container image storage used a separate directory.

The [host inventory](host.txt), [container configuration](containers.json), and
[LUKS status](luks-status.txt) record the deployed environment. Existing host
services and their configuration were preserved.

## Each comparison returns the same documents

The corpus has 100,000 bodies of exactly 4,096 bytes across 100 projects. Bodies
contain seeded technical vocabulary, punctuation, and a rare term every 5,000
rows. Another 20 rows contain all the rare term's fragments separately, forcing
exact verification to reject false positives. The corpus is compressible; its
storage ratio should not be generalized to arbitrary original documents.

Plaintext tables have a `pg_trgm` index over `lower(body)` and a lexical index.
Encrypted tables have a GIN array index over raw three-character fragments and
the same lexical projection. Encryption uses the proposal's exact PGP options:
AES-256, integrity checks, no compression, and S2K mode 3 with count 65,536. Reads
also verify an eight-byte record identity inside the encrypted message.

The client preloads synthetic per-record keys in temporary session tables before
timing. SQL latency includes query execution and returning the bodies. Vault
service calls, grants, key loading, and the full proposed envelope protocol are
outside this measurement. Production keeps all key ownership in Vault.

Searches select either 10,000 or 100,000 eligible IDs from the same physical
tables. Each case has one warmup, an `EXPLAIN ANALYZE`, and five timed repetitions,
with configuration order shuffled. The harness checks ordered IDs and returned
bodies across all four configurations. Initialization also verifies every body
against its decrypted counterpart on both storage variants. Another
[96 pattern checks](edge-parity.json) cover Unicode, punctuation, wildcards, and
escapes. Wrong payload keys and record identities were rejected. These checks
exercise the benchmark functions; application authorization and the complete
versioned envelope still require implementation tests.

These are warm searches on a shared host. Five samples support descriptive
medians; they do not establish production p95 or p99 latency. Host caches were
left intact. The [harness instructions](../../README.md) describe reproduction
and the separate pattern, concurrency, write, and storage tests.

After measurement, a [single restart check](restart-and-cleanup.json) reopened
LUKS with the supplied fixture key through private stdin in 188 ms. PostgreSQL
was ready 335 ms after container start. It retained 100,600 rows and returned the
checked body correctly. The check used no TPM operation and excluded Vault
service calls. Both benchmark containers were then stopped, filesystems unmounted,
LUKS closed, and loop devices detached. The images and root-only tmpfs fixture key
remain on .253 for reproduction; the key disappears on host reboot.

The corrected Optane run also finished with its containers and dedicated daemon
stopped, filesystems unmounted, and mapping and loops closed. Its
[cleanup record](optane-cleanup.json) is separate from the original run.
