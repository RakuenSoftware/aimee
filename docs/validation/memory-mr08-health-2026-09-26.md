# MR-08 retrieval health implementation evidence

Status: in progress. These are Go domain tests, not evidence that production
serving telemetry or the CLI is complete. MR-07 measured promotion remains open;
its default stays observe.

The metric foundation computes top-1/top-5 share, HHI, Simpson diversity, entropy,
normalized entropy, record/version populations, explicit previous-turn repeats,
retained contiguous runs with unknown boundaries, family counts, low-trust record
fan-out and verifier-labeled current-mode lifecycle re-entry. Empty populations
return null ratios. Historical rows are excluded from current lifecycle metrics.
Reports omit principal, record, task and family identities.

Sampling is at invocation level. Concentration describes the retained sample;
Horvitz-Thompson occurrence totals and their Bernoulli-design standard error are
separate estimates. Unknown sampling probability is invalid. Retained turns do
not imply adjacency across sampling, scope, task, retention or branch gaps.
Query fingerprints use HMAC-SHA-256 with a private key and namespace separator.

The private journal foundation has a 24-hour retention window, 256-attempt limit
and 384 KiB serialized bound. Whole-invocation sampling preserves exact retained
attempt and record counters separately from sampled record metadata. Verified
receipt observations reconcile unknown dispatch, network uncertainty, acknowledged
input and resolved unsent intent by immutable attempt binding. A late
acknowledgement replaces the population classification; retries cannot add a
second invocation. Capacity eviction reports incomplete coverage and prevents
an evicted prepared identity from re-entering through replay. Strict round-trip
validation rejects ambiguous persistence documents.

`TestHealth*` passed under the race detector, including hand-calculated values,
privacy of aggregate output, foreign-scope rejection, late/reordered observations,
serialized restart, whole-invocation sampling, overflow, retention and replay.
The namespace tests exercise domain binding; they do not yet demonstrate public
transport authentication or deployed PostgreSQL persistence.

Still required: durable owner integration; authenticated receipt-derived
collection and reconciliation; complete metadata and label availability handling;
CLI/JSON window queries; family fan-out and labeled contamination/recovery/
displacement metrics; baseline-relative alerts and authorized trace drill-down;
serving overhead, retention-growth and process/authorization acceptance.

The private PostgreSQL owner foundation now serializes journal creation and
updates under a per-principal transaction lock, with a 128-namespace quota.
A separate `memory-health` migration namespace uses the existing store capability.
The live PostgreSQL test used a non-owner role with no RLS bypass: 24 concurrent
attempts, duplicate acknowledgements, a fresh owner object, key stability,
rollback and principal/project isolation all passed under the race detector.
The migration dispatch and public transport remain unexercised; the test
installed the schema through its isolated administrative setup.

The receipt-derived importer and public `memory health` adapter are now implemented
and await deployed validation. The host derives the principal from authenticated
request context, captures a verified ledger head, enumerates at most 256 owned
requests and reads only rows through that head. Go reuses strict receipt
inspection and keeps missing query/task/family/trust/arm labels explicit. The
report separates exact retained stage counts from sampled concentration. CLI
text includes incomplete coverage, unknown denominators and collection loss.
Collection is default-off (`AIMEE_MEMORY_HEALTH_ENABLED=1` opts in); its optional
invocation sample probability is `AIMEE_MEMORY_HEALTH_SAMPLE_PPM` (1–1000000).
No provider payload replay is requested by the health collector.

Native WORM tests passed for principal isolation, snapshot boundaries, time
filtering and bounded overflow; CLI argument tests passed. Go race tests passed
for receipt conversion, stable sampling decisions during reconciliation, private
host invocation rejection and report-window validation. These unit tests do not
yet prove public HTTP authentication or the actual migration capability.

The local capacity benchmark (256 attempts) measured 840,909 ns/op,
881,653 B/op and 270,403 retained JSON bytes on an AMD Ryzen 7 7840HS. This is a
journal-only benchmark, not measured end-to-end serving overhead. Collection
currently happens during the health report, outside provider dispatch.

The actual private importer/report also passed against that restricted PostgreSQL
role: admitted-only input initially reported unknown dispatch; a late acknowledged
stage moved the same attempt to dispatched; repeated imports retained one count;
new owner objects recovered the journal; and a foreign principal received
not-collected. The aggregate exposed neither attempt nor record identities.
The native audit and CLI suites and non-PostgreSQL memory, execution-policy and
families race suites passed. Migration dispatch/public HTTP acceptance is still
pending, and MR-08 is not complete.
