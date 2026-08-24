# Evidence and lifecycle layer acceptance validation

Validated 2026-08-21 for PR #2831 against the proposal charter and P1–P9.
The implementation is default-on and uses one PostgreSQL transaction for each
mutation, its changeset items, and its evidence events.

## Test environments

- Native build and unit tests: Debian development workspace.
- PostgreSQL integration: Debian 13 LXC 9078, PostgreSQL 17
  with pgvector, one disposable database per run.
- HTTP exploratory E2E: the PR binary in the same LXC, isolated database
  `aimee_pr2831_http2`, isolated `AIMEE_HOME`, bearer authentication, port 18741.
- Frontend: TypeScript checks, production console build, and Vitest.

The PostgreSQL suite is [evidence-lifecycle-pg-test.sql](../../scripts/evidence-lifecycle-pg-test.sql)
and is run by [run-evidence-lifecycle-pg-test.sh](../../scripts/run-evidence-lifecycle-pg-test.sh).
It covers all 57 member-proposal acceptance checks; the six charter checks are
the corresponding cross-series assertions plus the two proposal checks below.

## Acceptance map

| Proposal | Checks | Executable evidence |
|---|---:|---|
| Charter | 1–6 | Per-kind fail-closed emitters, immutable compensating revert, document blast radius, structural outcome hash, `proposal-links-check`, reconciliation check |
| P1 | 1–6 | Authenticated HTTP boundary; closed operation and purge-ref constraints; all ten emitters unhooked in turn; rollback atomicity; N-row maintenance correlation; turn/retrieval-event correlation |
| P2 | 1–6 | One-operation changeset guard; byte-identical source changeset/events; lower-authority refusal; conflict and forced partial revert; irreversible purge refusal; multi-batch `origin_ref` grouping |
| P3 | 1–7 | Closed document states; invalidate without fact deletion; user fact re-verification and recall; named/countable blast radius; complete content-store purge and receipt; stale preview refusal; stable re-ingest identity/history |
| P4 | 1–6 | Explicit `dependencies:not-recorded`; essential dependency invalidation; reverse dependency projection; selective file-hash invalidation; selective policy-version invalidation; zero-drift reconciliation |
| P5 | 1–6 | Structural envelope hash unchanged; authenticated evaluator and closed outcome vocabulary; workflow-local preference/global contest; shared P4 staleness predicate; default rank weight zero; union into existing ranker-fit grouping path |
| P6 | 1–6 | Eight-value constraints/default; kind/authority independence; immutable episode with annotation route; policy governance force; exact legacy/migrated expiry-set comparison; mental-model governance gate |
| P7 | 1–7 | Count-only priority audit; changeset/event/revert chain; explicit computed markers; promotion evidence snapshot and actor; stale-head recomputation; kind-specific decision sets; control-web and handler ACL tests |
| P8 | 1–7 | Stable content hash/export-import; explicit unacknowledged widening refusal; exact dry-run/migration counts with rule detail; reversible narrowing; restore/quarantine rollback; stale dry-run refusal; P7-only provisional promotion |
| P9 | 1–6 | Trace is attached after ranking; exact dynamic-feature reconstruction; persisted rejection gate; scope isolation and P3 trace purge; sustained row/age caps and benchmark; exact outcome-to-trace fault join |

## Commands

```text
make -C src ../aimee-kb -j4
make -C src proposal-links-check
python3 scripts/check-proposal-reconcile.py
AIMEE_TEST_PG_ADMIN=postgresql:///postgres scripts/run-evidence-lifecycle-pg-test.sh
AIMEE_TEST_PG_ADMIN=postgresql:///postgres scripts/run-evidence-lifecycle-benchmark.sh
cd frontend && npm run check && npm run build:console && npm test -- --run
cd control-web && go test ./...
cd server-go && go test ./modules/control-web/policy/...
```

The reconciliation command is report-only repository policy. It completed with
no findings for this series; its warnings concern unrelated older proposals.

## Remote HTTP exploratory results

- Missing bearer was refused with 401; the verified owner bearer reached the
  operator surface and body-supplied identity fields were ignored.
- Multipart document ingest returned 201 and a stable document id.
- direct `DELETE /v1/docs/{id}` returned 409 and named the preview contract.
- invalidate preview and apply returned exact zero-impact counts for the seeded
  document and retained its shell as `invalidated`.
- purge preview from `invalidated` succeeded; an intervening mutation made the
  token stale and apply returned 409; a fresh preview purged content and returned
  a content-free receipt and changeset id.
- ontology export, review listing, work-outcome recording, trace persistence and
  scoped trace retrieval succeeded through `/v1/console/evidence`.
- the service deliberately had no embedder configured, so `/v1/health` reported
  degraded embedding readiness; the PostgreSQL-backed lifecycle routes above
  remained operational and were independently validated by the disposable-DB suite.

## Measured growth and latency

The reproducible benchmark is [evidence-lifecycle-benchmark.sql](../../scripts/evidence-lifecycle-benchmark.sql).
One run on LXC 9078 measured:

| Workload | Elapsed | Storage growth | Derived figure |
|---|---:|---:|---:|
| 10,000 memory mutations/events | 5,396.881 ms | 8,118,272 bytes | 811.827 bytes/event |
| 1,000 persisted traces / 5,000 result rows | 2,386.546 ms | 3,325,952 bytes | 2.387 ms/trace |

Trace persistence remained below the configured row cap; the integration suite
also injects over-age rows and verifies age pruning. Figures are measurements,
not performance guarantees, and include local PostgreSQL transaction/trigger
overhead in an otherwise idle test container.
