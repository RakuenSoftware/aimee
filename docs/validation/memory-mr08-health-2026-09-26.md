# MR-08 retrieval health implementation evidence

Status: in progress. Initial deployed collection and authorization checks passed;
new metadata capture, baseline and drill-down changes await deployed validation.
MR-07 measured promotion remains open; its default stays observe.

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

## Deployed collector and subsequent implementation

Candidate `1480664e0` passed the full T2 and T3 deployment matrices with actual
process exit 0 on both topologies. The native suites passed 99 and 81 checks;
five additional health checks passed on each topology. These exercised actual
HTTP authentication, lazy migration through the store bus, committed receipt
imports, ignored forged public identity/ledger fields, foreign OS-principal
isolation, explicit unknown metadata, and idempotent reconciliation after SIGKILL.
The immutable application image was
`sha256:9592125b08d953e4d06188aa88c0485343d0bec60f614c59093f12d6ff67e758`.
[Sanitized evidence](memory-mr08-health-evidence-2026-09-26/1480664e0-process-exits.json)
and topology-specific native/health checks are retained alongside image identities.
Both disposable topologies were cleaned; CT100 production was not replaced.
These results supersede the earlier pending migration/HTTP notes above.

Subsequent changes add optional serving-time keyed query capture, exact retained
native record-kind/trust metadata, per-kind and version concentration, family
fan-out, source-bound verifier label contracts and denominators, descriptive
adjacent-window comparisons, and explicitly requested scoped receipt references.
A baseline needs complete unsampled matching populations with at least 30
invocations in each window; harm alerts additionally need 30 known labels per
window and a 10 percentage-point adverse change. Concentration alone never alerts.
A baseline outside the 24-hour retention bound is explicitly unavailable.

Optional query capture uses a 50 ms cold-path budget and a bounded one-minute key
cache. Raw queries stay only in a bounded, single-use in-process token cache;
receipts retain a namespace-keyed fingerprint. Optional release metadata has a
separate 1 MiB pool and does not change required source-handle capacity or assembly
commitments. Overflow drops optional metadata while preserving serving. The
latest full non-PostgreSQL memory/execution-policy/families race suite passed;
the restricted live PostgreSQL capture test also passed across owner recreation.

A local warm-capture benchmark measured 1,366 ns/op, 1,254 B/op and 22 allocations.
The full 256-attempt journal benchmark measured 898,833 ns/op, 886,990 B/op and
270,448 retained JSON bytes. These are local component measurements, not provider
p95 or end-to-end overhead results. Collection remains opt-in on disposable test
stacks. No production baseline or wider rollout is claimed.

Remaining acceptance includes deployed validation of these subsequent changes,
end-to-end overhead/retention measurements, and owned task/turn/family/arm and
verifier metadata producers. Missing metadata and labels remain visible as
unknown or unmeasured; synthetic labels do not establish production usefulness.

## Capture acceptance failure and repair

Candidate `38771c3f1` built successfully and passed all 77 repository checks,
the full PostgreSQL memory race suite, and the focused enrolled-KB provider
boundary (352 checks) and native suite (99 checks). The health fixture then
failed because no serving query fingerprint reached collection. All 27 observed
assembly receipts had native selection metadata, but none had query context.
The failure is retained in the evidence directory; this candidate did not pass
MR-08 acceptance and T3 was not started.

The native composition transport omitted the task hint, so the Go owner's
single-use query token was never created on that path. The repair forwards at
most 4,096 bytes only for opted-in native composition, without copying query
text into the returned projection. Native transport tests cover disabled,
enabled and oversized inputs, and the full KB client and ingress suites passed.
The broader Go memory/execution-policy/families race suites also passed.

Further metadata work binds the host-issued task/session identity and ingress
turn to the exact receipt commitment. It does not infer predecessor turns across
requests or branches. Personal family capture reuses MR-04's scoped canonical
owner in a repeatable-read snapshot, accepts only the exact recalled revision,
and has a 50 ms batch deadline and 16-record ceiling. Shared, historical,
multiple-origin and unavailable ancestry remain explicit gaps. Deployed
validation of this repair and these additions is pending.

## Primary scope and turn identity follow-up

Candidate `1d4e4b792` passed the enrolled T2 provider boundary (352 checks) and
native suite (99 checks). The health fixture selected a different prepared
scope without captured query context and failed. Inspection found four captured
primary-scope queries and 43 records with canonical personal family metadata.
Selecting that actual primary scope and setting the CLI Unix endpoint explicitly
allowed all nine diagnostic health checks to pass, including isolation and
idempotent persistence after SIGKILL. This diagnostic does not establish full
acceptance for the next candidate.

Primary workers also lacked the host turn identity on their own thread. The
repair forwards a separate optional health identity and clears it after the
worker runs. It preserves globally unique retrieval-event IDs and binds the
health identity to the owned task and request, preventing reused presence turn
sequences from aliasing across requests. Targeted Go race tests and native ingress
tests passed. The updated deployed fixture requires owned task/turn metadata;
predecessor relationships remain explicitly unknown. Both topology runs for this
repair are pending.

## Interrupted guard transaction recovery

Candidate `533ba7408` built and passed all 77 repository checks and the full
PostgreSQL memory race suite and exporter. T2 then failed native recovery after
the deliberate Go-owner outage: the replacement could read memory but its next
provider run refused before dispatch. PostgreSQL showed an abandoned idle
transaction holding the send barrier after `memory_send_guard_end`, with later
completions blocked behind it. The independent store process retained that
transaction until its five-minute idle reaper ran. This explains a concrete
recovery failure; the failed candidate is not accepted.

Guard transactions now set a PostgreSQL-local five-second idle timeout. Losing
the Go owner between statement and commit therefore rolls back the abandoned
transaction and releases its row lock. It does not expire committed send leases
or assume dispatch completed. A PostgreSQL regression test observes the blocked
replacement acquiring the lock after timeout and proves the committed guard
survives the abandoned completion. That regression passed with the race detector.
Deployed recovery validation of this repair is pending.

The retained `533ba7408` T2 server separately passed all ten health diagnostic
checks, including owned task/turn identity, canonical family capture, CLI parity,
principal isolation and exact persistence after SIGKILL. The focused guard
PostgreSQL suite and memory health/source race suite passed. These results do not
override the failed native recovery gate. The exact two owned stacks were then
cleaned after exporting the diagnostic evidence.

Candidate `da4e67b44` passed the previously failing native recovery after owner
loss. T2 later stopped when the fixture attempted a shared mutation before the
sender's guard completion committed; the KB recorded SQLSTATE 55P03. That
refusal preserves the required send boundary. Provider interventions now use
one idempotency key and retry explicit error responses within a five-second
wall-clock bound, while withholding the retryable provider reply until the
mutation commits. Transport exceptions still fail the fixture. An isolated
16-check diagnostic passed shared insertion and private correction, each
preventing a stale second provider send. The full T2/T3 run is still required.

## Current deployed candidate: `2efe810ff`

Both owned CT109 topologies passed with the ordinary 32 KiB provider request
ceiling: T2 passed all 99 native checks and 10 health checks; T3 passed all 81
native checks and 10 health checks. Each fixture process exited zero. This
covers the repaired owner-restart recovery, bounded mutation intervention,
primary task/query metadata, canonical private families, authenticated CLI and
HTTP reports, scoped isolation, and exact retained counts after SIGKILL.
[Process and immutable image evidence](memory-mr08-health-evidence-2026-09-26/2efe810ff-T2-process-exits.json)
and [T3 process evidence](memory-mr08-health-evidence-2026-09-26/2efe810ff-T3-process-exits.json)
are retained alongside the individual checks and image identities.

T2 retained 52,806 bytes across two health namespaces (largest 46,452 bytes);
T3 retained 49,229 bytes (largest 42,876 bytes). Six report calls per topology,
including calls after restart, took 0.905–1.139 seconds for T2 and 0.729–0.952
seconds for T3. These are small synthetic workload observations, not a serving
p95 overhead comparison or a production growth forecast. The initial T3 launch
stopped before topology creation at the disk-space precondition; after removing
specific obsolete build-cache entries, the independent T3 run passed. Both
successful topologies were removed after evidence export. CT100 was unchanged.

MR-08 remains in progress: the event metadata gaps and serving-overhead
acceptance still need resolution. These deployed passes do not supply MR-07's
activated paired-workload quality or cost measurements.

## Final positions from the rendering owner

Optional health metadata now retains each delivered record's one-based position
within its exact rendered projection, bound to that projection's digest. Typed,
fact and preview projections retain their final order before source-fence
deduplication. The native renderer records positions while writing rows, so
unversioned rules/rows still occupy their actual positions. Repeated appearances
retain multiple positions without changing record-level concentration.

Refresh removes old native positions and preserves retained ingress positions;
explicit appends preserve both projections. Omitted versions cannot inherit
positions, and malformed or absent positions retain the visible `final_rank`
gap. No global rank is inferred across independently ranked projection blocks.
The existing optional metadata pool and receipt bounds still apply.

The focused health/native/source-release race suite passed in 2.95 seconds,
including four new position regressions and the existing serving-content
invariance check. Deployed validation of this addition remains pending; the
ongoing serving-overhead experiment uses the earlier `be46915db` image.

## Native serving observations on `be46915db`

A counterbalanced off/on/on/off run completed 32 measured requests per arm,
with three excluded warm-ups per block and zero measured request failures.
Each request used a fresh session, settled index, the same complete typed
requirement and a fixed single-response synthetic provider. The manifest and
harness hashes were frozen before collection. All four blocks independently
confirmed the expected presence or absence of optional receipt metadata. The
controller exited zero and restored the original disabled environment; the
Server remained healthy.

| Collection | Mean | p50 | p95 |
|---|---:|---:|---:|
| Off | 3.093 s | 3.075 s | 3.236 s |
| On | 3.079 s | 3.072 s | 3.245 s |

The observed p95 ratio was 1.00258 (+0.26%). This small synthetic workload does
not establish a production latency distribution or a speedup. Other validation
activity overlapped part of collection, so it does not isolate CPU cost. It
also predates the new final-position metadata. The explicit provider ceiling
was 65,536 bytes. [Manifest](memory-mr08-serving-overhead-evidence-2026-09-26/be46915db-manifest.json),
[all observations](memory-mr08-serving-overhead-evidence-2026-09-26/be46915db-progress.json),
[summary](memory-mr08-serving-overhead-evidence-2026-09-26/be46915db-summary.json)
and [capture checks](memory-mr08-serving-overhead-evidence-2026-09-26/be46915db-capture-checks.json)
are retained with the exact harness sources.

The on-block health imports reported no failed requests or truncation. The
namespace retained 144,634 bytes, including earlier fixture receipts; this is
not incremental bytes per measured request. Earlier network-uncertain evidence
remained classified separately. The first strict overhead attempt aborted on
an incomplete native request and restored the environment; it is not a passing
comparison. A four-request enabled diagnostic then passed. The subsequent
frozen comparison retained every measurement and would have made any measured
failure ineligible, without silently retrying or dropping it.
