# G0: Go memory ownership closeout

Date: 2026-09-19. PR: [#2983](https://github.com/RakuenSoftware/aimee/pull/2983).

G0's language and ownership cutover uses one Go memory implementation for Server
and KB, with placement-specific storage and permissions. Module-side producers
and consumers use the Go binding to the existing C event bus. The bus remains C.
Neither memory tree contains native source/header files or cgo wrappers, and the
memory descriptor has no native source, header or test registration.

## Reviewed dispositions

The immutable native inventory pins 24 original files and 298 API symbols.
The [ownership ledger](../../../tests/baselines/modules/memory-ownership.json)
records each file's replacement paths and each API's disposition, including the
owner, contract, reason, evidence and cutover dependency. It also classifies 88
tracked external files and two generated headers. Generated headers may be absent
before a build; their output hashes and generator/input hashes remain checked.
Unreviewed callers and changes to reviewed external adapters fail the gate.

Go owns memory policy, retrieval/composition, persistence orchestration,
extraction, gates, embeddings, evaluations and live memory text/JSON views.
The unlinked `cmd_memory*` console family and unused native record wrappers are
retired. Shipping served CLI, MCP and HTTP routes preserve full owner envelopes,
exact IDs and explicit failures. Native host authorization, audit, connection
handling and forwarding remain with their external owners. No C memory fallback
is permitted when the Go owner is unavailable.

`check_memory_go_only.py` retains its original broad, all-native-caller report.
It intentionally still finds C host/protocol references. That report is not a
zero-C-memory implementation count and has not been relaxed to certify G0.
The module-only gate, retired-policy boundary guard and reviewed ownership ledger
jointly enforce the user-requested language boundary.

## Validation evidence

The migration has passed these local checks against its production Go owner:

- `check_memory_c_boundary.py`, `check_memory_go_only.py --module-only`,
  `check_memory_ownership.py`, and their negative regression tests.
- `CGO_ENABLED=0` module/probe builds and real C-bus conformance in both placements,
  including killed-provider refusal and process restart.
- Memory, evaluator and module race suites against PostgreSQL/pgvector; scoped
  runtime-role replay at both 384 and 1024 dimensions.
- Native CLI/Server/KB builds and focused memory, agent, session and MCP transport
  tests, including full content, large IDs, counts and explicit owner refusal.
- HTTP integration: 148/148 configured checks, including persistent private CRUD,
  owner outage and restart. Twenty-five checks skip because their optional
  services are absent; this is not certification of those full-stack journeys.
- Full native unit-test target compilation/linking; repaired host-dispatch tests
  and PostgreSQL trajectory export using the real Go screening fixture.
- PostgreSQL P1 isolation gate, including privacy erasure and both retention
  paths with asserted durable Go indexing cleanup jobs.
- Complete-history secret scan; its sole new finding was a test-only PEM opening
  marker, recorded by exact historical fingerprint without a path/rule exemption.
- DB2 packaged replay, declaration/link closure and source-boundary checks;
  isolated DB2 and PostgreSQL export builds. These preserve external integration
  while DB2 retirement is deferred.

Clean CI exposed stale native test link dependencies, generated-header ordering,
a command-help loop bound, generated ownership findings and export/inventory
bookkeeping. The PR includes the corresponding repairs and the privacy-definer
queue grant required by Go indexing invalidation. The subsequent
[fresh-environment release validation](../../validation/memory-g0-2026-09-19.md)
records real Docker deployments on a new `.253` guest, HTTP/CLI/MCP placement
and outage checks, concurrent large-ID exploration, published upgrade/rollback,
and a real-provider paired quality/latency comparison. It also records CI status
for the tested implementation. Exact-ID transport, failure classification and
retryable PostgreSQL initialization repairs found by that run are included.

## Deferred work

[Complete DB2 retirement](db2-as-a-go-module.md#future-proposal-todo-retire-db2-completely)
is a future proposal TODO. It includes non-memory consumers, storage/data
migration, permissions, upgrade/rollback and deletion of DB2 builds, descriptors,
providers and compatibility layers. It does not block this memory-only cutover.

G0 does not certify historical ranking/performance parity, the complete
adversarial release matrix, or MR-01–18. Those retain their acceptance criteria in
the [delivery tracker](memory-reliability-delivery.md) and
[retrieval compatibility decisions](memory-reliability-retrieval-compatibility.md).
