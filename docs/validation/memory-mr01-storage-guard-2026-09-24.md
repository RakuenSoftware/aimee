# MR-01 tracked-source send guard — 2026-09-24

The shared and private PostgreSQL owners now support a committed, bounded send
lease. Acquisition takes an exclusive lock on a per-store barrier before checking
retained source versions and eligibility. Semantic writes hold compatible shared
barrier locks until commit, and refuse while a lease is active. Usage-counter
updates remain available. Lease rows survive connection loss; explicit release
removes them, and the fixed five-second storage interval bounds orphaned leases.
Acquisition requires synchronous commit. Tokens and the barrier are not readable
or writable by runtime roles; security-definer functions put the trusted schema
before temporary tables. Shared lease requests require verified service or
unscoped owner authority, separately from ordinary scoped source inspection.

The provider HTTP paths acquire after connection and request construction, before
writing, and release before reading the response. Their monotonic acquisition and
write budget is 4.5 seconds, beginning before either owner is contacted. Both
owners must acknowledge acquisition. Current eligibility is also checked through
a conservative five-second future horizon. Refused acquisition rolls back its
lease. Every buffered retry and streaming request uses this boundary; ordinary
nested owner HTTP calls do not inherit it. Existing prepared/admitted receipts
remain history when a later guard refuses the attempt.

The [targeted race tests](memory-mr01-storage-guard-2026-09-24/targeted-race.txt)
pass, covering shared/private observations, expiry within the send window,
restricted-role mutation serialization, normal concurrent writes, counter updates,
multiple leases, rollback, old repeatable-read snapshots, connection loss,
authorization and bounded expiry. A [temporary-table bypass reproduction](memory-mr01-storage-guard-2026-09-24/temp-shadow-before.txt)
is retained; the same case now refuses the mutation. Native tests pass for
[buffered retries](memory-mr01-storage-guard-2026-09-24/http-retry.txt),
[all streaming routes](memory-mr01-storage-guard-2026-09-24/wire-fence.txt) and
[real socket writes](memory-mr01-storage-guard-2026-09-24/http-send-guard.txt).
The [private migration manifest](memory-mr01-storage-guard-2026-09-24/migration-manifest.txt)
remains ordered, complete and append-only (new version 34).

Two full-suite setup failures are retained: [DDL lock ordering](memory-mr01-storage-guard-2026-09-24/first-full-migration-lock-failure.txt)
and a [missing fixture execute grant](memory-mr01-storage-guard-2026-09-24/second-full-fixture-grant-failure.txt).
The shipping DDL is now installed before the replay, and the restricted fixture
explicitly grants the guard functions. The [final full memory race suite and C export build](memory-mr01-storage-guard-2026-09-24/race-export.txt)
passed in 271.006 seconds and 5.848 seconds. The [167-check HTTP candidate run](memory-mr01-storage-guard-2026-09-24/checks.json)
passed on `9da4735e6`, including both real owner audiences and storage mutation
attempts during admission. [Image identities](memory-mr01-storage-guard-2026-09-24/image-identities.json)
record the application and dependencies. The [native worker run](memory-mr01-storage-guard-2026-09-24/native/native-async.json) also passed all 53 checks on the same candidate.

This does not certify MR-01 A5. Process-level concurrent-release and restart
validation remains, along with unversioned channels and the remaining policy and
revocation context. The bounded lease also depends on the database clock and host
write scheduling staying within the timing margin; arbitrary clock steps or
process suspension across the final deadline check require separate treatment.
All draft schema/application work stays on CT109. CT100 remains on released
0.4.5; no new migration has been applied there.
