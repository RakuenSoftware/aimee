# Memory retrieval performance follow-up

Date: 2026-09-20. PR: [#2983](https://github.com/RakuenSoftware/aimee/pull/2983).
Memory/Go-binding implementation: `bddab521be`.
Cancellation regression follow-up: `ec6b030926`.

The optimized Go path has **233.5 ms p95**, versus **250.9 ms**
for the original pre-G0 baseline in the same fresh paired run: **6.9% lower**.
All three measured rounds favor the candidate at p95. Recall@5 remains 100%,
MRR remains 0.9698, and every full ranked list matches the preceding Go
implementation. Median latency is slightly higher than the pre-G0 baseline.

## Changes

- Memory takes one lazy configuration snapshot per request. Retrieval lanes
  share that snapshot, including any read error; the next request sees updated
  settings. This removes repeated configuration service calls without a
  process-wide policy cache.
- Semantic recall combines catalog existence checking with the rebuild-lock
  operation, then reads active version, command, dimension and serving identity
  together. The metadata statement still runs **after** lock acquisition, so a
  cutover that held the exclusive lock is visible in the new statement snapshot.
- The Go module runtime publishes a completed handler response when its completion
  channel fires, instead of waiting for the shared-ring polling timer.
- The Go concurrent requester wakes when a call is sent, bounds response polling
  to 1 ms while calls are pending, and retains bounded backoff when idle.

The last two changes are in the **Go binding** used by memory and its Go service
providers. The C bus, protocol, ownership and authorization remain unchanged.
Both live embedder identity checks still run. Whole-record/unit recall, scope,
current-input, lifecycle and ranking predicates are unchanged. No query-vector or
retrieval-result cache was introduced, and no semantic lane was disabled.

## Fresh shipping-image comparison

The shipping `Dockerfile.server` image was built in the isolated CT 9498 on
`.253` from the committed source, with `WITH_VSCODE=0`. The guest was reused from
G0 validation, but every final topology and paired arm had fresh application
identities and PostgreSQL volumes. Previous completed topology containers and
networks were removed to free Docker address pools; their data volumes and
recorded evidence were retained.

Candidate image: `sha256:742e5a729d0e114112ae43d7975e53400c238751ce0522cafe782f57ceda45ba`.
The same pinned PostgreSQL and real Bekko-a25m image from the
[G0 report](memory-g0-2026-09-19.md) served both arms at 384 dimensions.
The 50-fixture, 105-query corpus and raw queries were unchanged. The pre-G0
baseline remains `bedabac667c5c00f9f14fe6f47efd3b369a46922`.

One full warmup and three measured rounds per arm alternate arm order. All
other test stacks were stopped before timing. Reported request latency includes
Docker exec/Python overhead identically on both arms. The baseline uses its
existing lexical/graph route; the candidate additionally performs versioned
whole-record/unit semantic retrieval. This measures the actual serving pipelines,
not equivalent C-versus-Go computation.

| Metric | Pre-G0 baseline | Optimized Go |
|---|---:|---:|
| MRR | 0.7429 | 0.9698 |
| Recall@5 | 0.7381 | 1.0000 |
| NDCG@5 | 0.7392 | 0.9777 |
| Request p50 (ms) | 205.8 | 208.6 |
| Request p95 (ms) | 250.9 | 233.5 |
| Measured queries | 315 | 315 |

Per-round p95 (baseline → candidate): **250.2 → 234.7**, **248.9 → 230.5**,
**250.9 → 239.3 ms**. The [full paired receipt](memory-performance-2026-09-20/paired-retrieval.json)
includes every query, ranked list, timing and preparation result.

All **420 candidate ranked lists** (one warmup plus three measured rounds of
105 cases) match the prior Go candidate’s full ordered corpus IDs, not only its
aggregate recall. Its quality metrics are unchanged. The old 446.1 ms p95 Go
release result was a separate historical run; the new 233.5 ms figure is about
48% lower, while the simultaneous pre-G0 comparison above is the stronger
latency control.

## Concurrency and idle cost

Compared the retained pre-optimization Go release with the fresh optimized Go
stack using the same 105 query cases and eight concurrent workers:

| Metric | Previous Go | Optimized Go |
|---|---:|---:|
| HTTP p50 (ms) | 329.8 | 141.0 |
| HTTP p95 (ms) | 379.5 | 169.5 |
| Queries/sec | 23.2 | 52.6 |
| Application CPU seconds, including warmup | 1.796 | 1.365 |
| Total application + PostgreSQL + embedder CPU seconds | 5.970 | 5.424 |
| Idle CPU cores, all three containers | 0.0468 | 0.0472 |

Throughput increased 2.27x, concurrent p95 fell 55.3%, and aggregate CPU used
for the same workload fell 9.1%. Idle CPU is effectively unchanged in this short
sample. Every returned full ranked list in both arms still matches the original
Go reference. [Raw concurrency and idle receipts](memory-performance-2026-09-20/concurrency-and-idle.json).

The [one-off measurement harness](memory-performance-2026-09-20/concurrency-harness.py)
is retained with the evidence. It reads or generates test credentials at runtime
and records no credential values. Idle CPU is measured from Docker cumulative
CPU counters over 30 seconds after a 30-second startup cooldown. One CPU core
means one second of CPU time per elapsed second. Concurrent latency is measured
inside the container around HTTP; it excludes Docker-exec startup and must not be
compared directly with the paired table’s latency boundary. Each arm runs ten
warmup queries followed by the same 105 cases with eight workers. CPU totals for
that phase include the warmups. These are bounded comparisons, not saturation
or sustained-load capacity tests.

## Validation

The full Go memory race suite and restricted-role PostgreSQL replay passed at
both 384 and 1024 dimensions. Coverage includes hidden scopes, stale/moved
vectors, expiry/suppression, wrong dimensions, provider identity changes and
SQL failures. The new policy-snapshot regression requires one settings read
within a request and verifies that a subsequent request sees a changed floor.

Go binding race tests pass. Virtual-clock regressions verify bounded idle polling,
immediate wakeup for a ready response, bounded polling while a response is
pending, and immediate publication when a handler finishes. They exercise timer
ordering without wall-clock timing thresholds. Full cross-language conformance
also passed, including the actual C host, both memory placements and process
termination/restart. Module-only, retired-C and ownership gates remain green.

Fresh T1 KB, T2 separate Server/KB and T3 standalone Server runs passed
**316 recorded verdicts**, including suite wrappers. The T3 exploratory tests
include exact int64 IDs, concurrent Unicode writes, restarts and recovery.
[Sanitized topology verdicts](memory-performance-2026-09-20/topology-verdicts.json)
retain each check name and outcome. The shipping image for these runs is the
performance implementation above.

CI on that implementation exposed a previously timing-dependent workflow
cancellation defect: the runner failure write could replace an operator’s manual
pause with an automatically retriable pause. `ec6b030926` preserves an existing
pause or terminal state while accounting for runner spend. The strengthened
cancellation regression fails against the previous binary and passes ten
consecutive runs with the fix. The full live workflow store/API/engine race
suite also passes. This follow-up changes no memory or Go-binding code; its
validation is separate from the performance-image receipts.

All **59 reported CI checks passed** on implementation revision `ec6b030926`,
including the full PostgreSQL/workflow shard, T1/T2/T3/T2-LUKS deployment and
upgrade/rollback suites, complete script regressions and ASan/UBSan.
[Implementation CI receipt](memory-performance-2026-09-20/implementation-ci.json)
links every check. The following evidence-only commit is independently subject
to the PR gate.

The prior 446 ms candidate p95 and 251 ms pre-G0 p95 are the historical G0 release
run, not simultaneous measurements with this follow-up. This small synthetic
corpus and one hardware environment do not establish production-scale capacity
or complete MR-18 adversarial acceptance. Full per-query receipts and the existing
compatibility decisions remain authoritative for the tested scope.

Runtime artifacts remain under CT 9498:
`/opt/aimee-2983-evidence/performance-bddab521be`.

## Reproduce the paired run

Build the two revisions with `Dockerfile.server`, then run on an otherwise quiet
Docker host with the same pinned providers:

```sh
AIMEE_POSTGRES_IMAGE=aimee-postgres:pr2983 \
AIMEE_EMBEDDER_IMAGE=ghcr.io/rakuensoftware/aimee-embedder-a25m@sha256:b03199bee881bf632f7194f472de7bc370e66d16b7215bb6aa506fb2b1510209 \
python3 tests/e2e/memory-paired-retrieval.py \
  --baseline aimee:memory-baseline \
  --candidate aimee:memory-perf-bddab521be \
  --baseline-revision bedabac667c5c00f9f14fe6f47efd3b369a46922 \
  --candidate-revision bddab521be1194bb200701d9949ebb0abaeb5b82 \
  --output /tmp/memory-paired-performance
```

The harness creates disposable identities and databases, waits for real-model
indexing and version activation, and records the immutable image and corpus
identities. Its default cleanup removes only its own stacks.

Two preparation attempts were discarded before measurement: the local
coordinator accidentally stopped the first pair along with completed topology
stacks, and retained networks exhausted Docker’s address pool on the second.
The completed run used new stacks after cleanup; no timed samples came from
those interrupted attempts. Their logs and volumes were retained in the guest.
