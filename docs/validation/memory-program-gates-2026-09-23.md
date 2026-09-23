# Memory program gate follow-up — 2026-09-23

This PR #2990 batch advances MR-01, MR-06 and MR-18. The frozen 123-clause
inventory is unchanged; none of the 18 proposals is certified complete here.

## MR-01: negation candidates obey current validity

The optional negation SQL arm previously filtered lifecycle and suppression but
omitted valid time. The [restricted-runtime-role fixture](memory-program-gates-2026-09-23/negation-before.txt) reproduced expired
records entering recall and crowding out eligible candidates at the 64-row arm
cap. The query now uses the existing Go-owned current eligibility predicate
before that cap. Policy identity advances to current-validity-v8.

The fixture covers visible and exact-project recall, shared and hidden scopes,
70 expired candidates, future validity, the exact upper/lower boundary,
suppression, supersession, quarantine, deletion, revocation and rejection.
Malformed nonempty time input fails explicitly; a successful unbounded/valid
read remains available after correcting it. Disabled negation behavior and
positive-query ordering retain their prior contract. This is a serving repair,
not a change to maintenance or historical-inspection admission.

## MR-06: observed returned-candidate ranking

Diagnostics capture request-local ranking steps at the operations that execute
them: deduplicated RRF arm ranks/votes, candidate-order score resets, negation
overlap and optional PageRank addition. Dense-only/graph-only candidates have no
fabricated lexical vote. Each step describes its own score; summing all steps
would double count, so persisted feature contributions include only the final
stage. Prior stage ranks/scores remain zero-weight numeric metadata in the
existing trace schema. No database migration is needed.

Diagnostic responses label observed evidence separately from the legacy
exact-ID text-match estimate. MCP retains its numeric scores map. Automatic
ingress previews explicitly disable capture and preserve their established score
and rendering commitments. Immutable copied steps prevent nested fusion from
changing a prior response. No process-global mutable trace state was added.

This does not claim raw SQL/cosine score capture, a full candidate-universe trace,
rejected-candidate reasons, source-version binding or durable provider dispatch.

## MR-18: standalone owner packaging

The prior lifecycle commits omitted four Go source/test entries from the memory
descriptor. The existing descriptor ownership gate reproduced the omission.
All four entries are now declared. A new export test creates an independent
memory repository, compares every Go owner source/test against its original,
builds the actual process and runs lifecycle/ranking regressions there.

## Validation and limits

- The eligibility regression failed before the predicate repair and the full
  memory PostgreSQL suite passed after it.
- The [final full memory PostgreSQL/race suite](memory-program-gates-2026-09-23/race-tests.txt)
  passed in 133.6 seconds, including persisted trace and MCP compatibility checks.
- Native ingress and KB HTTP suites passed with the Go runtime fixture.
- Concurrent rank tracing tests preserve order, deduplicated votes and prior
  response evidence; disabled tracing is identical to the original fusion.
- The full exporter test file passes (19 tests, one environment-dependent skip);
  the descriptor test file passes (47 tests).
- Three local 64+64 candidate fusion samples retain the same 58,552 B and 136
  allocations with capture disabled. Median disabled time was 41.6 microseconds
  versus 41.7 for the compatibility-call wrapper; enabled diagnostics used
  75,776 B, 337 allocations and 52.0 microseconds. These are component measurements, not
  whole-request latency or MR-18 release certification.
- Memory ownership/C boundary, module-bus boundary and descriptor validation pass.
  The generic repository-wide Go import checker reports 64 findings; all were
  compared with the preceding commit and are unchanged. It is not a green
  repository-wide boundary result.
- The broad script discovery run was stopped during the unrelated DB2 declaration
  ledger tests after reporting other errors. Fail-fast reproduction identified
  missing local libpq pkg-config metadata in a runtime-bundle fixture. The task
  toolchain was corrected and all 10 runtime-bundle tests passed; the broad run
  remains incomplete. Relevant export/descriptor files passed directly.

The shipping 0.4.5 application, database and embedder on CT100 remain separate
from the disposable test PostgreSQL instance. All three production services remain healthy. A fresh paired thin-client
store/get/delete/not-found check passed and its temporary record was retired. No draft PR image replaces the
released 0.4.5 deployment.


Raw [export](memory-program-gates-2026-09-23/export-tests.txt),
[descriptor](memory-program-gates-2026-09-23/descriptor-tests.txt) and
[ranking benchmark](memory-program-gates-2026-09-23/ranking-benchmark.txt)
results accompany this record.
