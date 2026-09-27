# MR-08 retrieval health implementation evidence

Status: complete. Implementation and acceptance validated on 2026-09-26;
optional collection remains disabled by default.
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
invariance check. The PostgreSQL-backed focused race suite subsequently passed
in 4.07 seconds. All 77 lint checks ran; the sole failure was the new file's
module registration. After registering the source and its test, that schema
check passed, as did the added bounded-position regression.

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

## Deployed positions and quiet serving comparison on `71208d807`

Both owned CT109 applications upgraded from `be46915db` to `71208d807` with
their stores, Vaults, workspaces and enrolled identities preserved. Both became
healthy. The immutable application image is
`sha256:e1c8283fdb70ddc4dfe10d49f01504e7e2d88c40585d733421bf152ec224a6b6`.
The native position fixture passed all 15 assertions and exited zero. It stored
a private identity through the authenticated API, verified its exact owner and
revision, and matched the bytes of two actual provider requests to committed
receipts. Both native and typed projections retained valid positions. The
authenticated report imported both dispatched attempts without loss and cleared
their `final_rank` gap. Unsupported predecessor metadata remained unknown.
[Checks](memory-mr08-serving-overhead-evidence-2026-09-26/71208d807-positions-checks.json),
[captured positions](memory-mr08-serving-overhead-evidence-2026-09-26/71208d807-positions-captures.json)
and [health summary](memory-mr08-serving-overhead-evidence-2026-09-26/71208d807-positions-health-summary.json)
are retained with the exercised harness sources. The temporary identity was
deleted and the model roster restored.

A subsequent quiet off/on/on/off run used eight measured requests and three
excluded warm-ups per block. No other CT109 builds or validation suites ran
during collection. The corpus, synthetic provider, context limit and settlement
procedure matched the preceding experiment; the manifest and harness hashes
were frozen before collection. All 32 measured requests completed, with zero
failures in either arm. Each block verified optional capture was actually off
or on.

| Collection | Requests | Mean | p50 | p95 |
|---|---:|---:|---:|---:|
| Off | 16 | 3.158 s | 3.166 s | 3.358 s |
| On | 16 | 3.185 s | 3.161 s | 3.318 s |

Mean latency increased by 26.7 ms (about 0.85%). The empirical p95 ratio was
0.9881; with only 16 observations per arm, this nearest-rank p95 is the sample
maximum and does not establish a speedup or a production distribution.
[Manifest](memory-mr08-serving-overhead-evidence-2026-09-26/71208d807-manifest.json),
[observations](memory-mr08-serving-overhead-evidence-2026-09-26/71208d807-progress.json)
and [summary](memory-mr08-serving-overhead-evidence-2026-09-26/71208d807-summary.json)
are retained. The namespace held 220,947 bytes including earlier fixture
receipts; no per-request or production growth rate is inferred. The controller
exited zero, restored collection to off, and left the upgraded Server healthy.

MR-08 remains open for the other metadata producers, including owned predecessor
links and the incomplete arm, release/verifier and provenance metadata paths.
These observations do not supply MR-07's independent paired quality/cost gate.

## Typed assertion metadata producer

After final ingress repacking, optional health capture now copies the retained
assertion's kind, lifecycle state, confidence class and validity interval from
the version-checked owner projection. It records each actual lexical, vector
or semantic-graph arm rank, raw score and reciprocal-rank contribution, with
`assertion_rrf_k60_v1` provenance. The whole-candidate fused score remains a
separate field. Ranking and telemetry share the existing contribution formula;
this refactor does not change its value or ranking behavior.

The importer validates bounded, unique arms and checks contributions against
the fused total. Missing or malformed evidence retains the arm-contribution gap.
High confidence does not become a trust or release-safety label. Raw assertion
text, evidence spans and parent identities are excluded from this capture.
Historical/current classification still comes from the retained read policy.

Four new regressions cover final version matching, budget omission, serving
commitment parity, metadata privacy, malformed arm contributions and receipt
import without invented safety labels. Focused Go race tests passed in 2.715s;
PostgreSQL-backed health/ingress/assertion/native/source-release checks passed
in 4.273s. All 77 lint checks passed.

Candidate `be3f2577c` subsequently passed all 23 deployed checks, actual exit
zero. Two native requests matched their committed provider receipts, and health
import completed without failures or truncation. Kind and final-position gaps
were absent for this fixture; unavailable metadata stayed explicit. The
[retained evidence](memory-mr08-typed-evidence-2026-09-26/README.md) includes
checks, capture summaries, health output, image identities and harness snapshots.
Both isolated applications are healthy on this candidate. Optional collection
is restored off. The earlier measured serving-overhead comparison applies to
`71208d807`, not this new slice; no new latency result is claimed.

## Remaining serving-evidence producers

Native turn adjacency now belongs to the receipt owner. A host worker closes its
outer turn with an opaque, request-bound handle. Only a completed turn with
durably acknowledged provider input can become the next turn's predecessor.
Retries keep the same link. Overlap, uncertain/unconfirmed delivery, expiry,
restart, foreign finish calls and capacity exhaustion cannot invent adjacency.
The optional cache is bounded to 64 tasks and 16 turns per task, with a 15-minute
expiry; durable receipts retain links already issued. Missing links stay unknown.

Native ranking metadata travels outside the budgeted recall body and joins only
the exact placement, owner, record and revision. It preserves ordered score
stages and actual contributions without adding scores from different stages.
Unranked identity/preference/commitment selectors are identified separately.

Canonical family capture now supports shared roots and multiple established
origins. Separate bounded owner reads follow the required serving transaction,
so optional lookup failure cannot abort that transaction. Capture checks exact
revisions and retains no partial family list when ancestry is incomplete.
Historical lineage unsupported by the canonical owner remains unknown.

The lifecycle verifier uses successful v2 source-owner guard admission, bound
to the prepared receipt and exact source check/digest, within five seconds of
that check. Reports name `source_owner_guard_v2` and its source-admission-only
scope. This does not label truth, usefulness, arm contamination or fusion
recovery. Those semantic metrics still require explicit selection-verifier
labels; absence is unmeasured, not a negative label.

Other typed record kinds preserve their observed kind/state/provenance. Only
explicit untrusted-data or existing owner authorship classifications supply
trust labels; neither confidence nor derivation alone establishes trust.

The full PostgreSQL memory race suite passed in 417.589s, followed by the
exported-owner build check in 7.009s. Final focused placement/provenance tests
passed in 10.083s. The native ingress fixture and all 77 lint checks also passed.
Deployed and final overhead validation remain before closing MR-08.


### Final deployed validation correction

The first `9cfe9552f` replay exposed a real ordering gap: preparation uses
preflight revalidation, while the v2 send guard is acquired later at the socket
write. Its proof therefore cannot be attached to the earlier assembled receipt.
The owner now binds the later guard check to the pending prepared attempt and
persists it in the existing dispatch-started event. Verified ledger inspection
carries that event into health import. Attempt, source digest, preparation check,
guard check and timestamps must agree; optional proof capacity never prevents
the required dispatch observation. Regression coverage exercises the actual
prepare → guard → write-observation → verified-ledger → health-import order.

The deployed diagnostic also confirmed completed-turn adjacency and private
families. Shared canonical family metadata matched the first retained shared
source; the second native selection omitted that source entirely. The harness
checks families against each final source set, not the preceding turn's set.
Repeated fixture setup initially reused erased shared content and correctly hit
the surviving erasure intent. Subsequent fixtures use unique keys and content.
No product eligibility or erasure rule was relaxed.


## Final acceptance

Candidate `4d0870c70` passed all 34 deployed serving-evidence checks, with two
verified provider requests and zero collector failures or truncation. Receipt
inspection/import retains the actual dispatch-time source guard proof. The first
turn has unknown adjacency; the second links the completed durable first turn.
Family metadata follows the exact final source set, including shared roots.

The quiet off/on/on/off comparison completed 32 measured requests plus 12
excluded warm-ups with zero measured failures. Off/on means were 3.730/3.789s,
p50s 3.688/3.750s, and p95s 3.936/3.966s: 59.1ms (1.59%) mean overhead, p95 ratio
1.0075. With 16 observations per arm, this p95 is the observed maximum. No
real-model or production distribution claim follows. Retained state was
292,150 bytes including earlier receipts, below the 384KiB namespace bound.

The controller exited zero, restored collection to unset/off on both owners,
and verified both remained healthy. Existing stores, workspaces, Vaults and
enrollment survived each isolated upgrade. The final correction passed focused
race tests (1.647s), PostgreSQL checks (4.637s), and all 77 lint checks, following
the full memory race/native validation documented above.

[Final evidence and harnesses](memory-mr08-final-evidence-2026-09-26/README.md)
include the initial failed checks and their corrections. Unsupported labels,
historical lineage and insufficient baseline populations remain explicitly
unknown. They are data-availability limits, not fabricated successful outcomes.
MR-08 implementation and acceptance are complete; widening optional collection
and MR-07 quality/cost promotion remain separate decisions.
