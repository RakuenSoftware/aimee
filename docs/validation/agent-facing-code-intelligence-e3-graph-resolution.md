# Agent-facing code intelligence E3: exact graph resolution

Validated: 2026-07-30

E3 removes authoritative `%file_path%` matching from both canonical and direct DB2 blast-radius
paths. The shared extractor/query identity layer maps Python file paths to dotted modules and
resolves normal, explicit `from`-pair, package-relative, and `__init__` forms against the importing
file. Non-Python imports remain exact slash-normalized identities; additional language resolvers can
be added without reintroducing substring authority.

The graph result now proves that the target resolved in the requested current generation. Local
import edges are followed by uniquely resolvable direct calls, uniquely resolved co-edit
projections, and then route-gated cross-project imports. Edge deduplication is project plus path, so
same-path files in different projects stay distinct. Every edge carries provenance, confidence,
project, generation, and freshness through HTTP, the KB client, CLI JSON, and blast-preview JSON.

Validation performed:

- extractor unit coverage for `import app.dates`, `from app import dates`, relative imports,
  package `__init__`, and `app.dates_extra` collision rejection;
- SQLite-backed canonical index coverage for the fixed `app/dates.py` fixture, three import
  dependents, one call-only dependent, merged provenance, exact dependencies, and unresolved-target
  failure;
- route-gated cross-project coverage proving an identical un-routed distractor is excluded;
- unique co-edit projection coverage with explicit `projection` provenance, including a combined
  local-projection/cross-project fixture that proves every local edge precedes the external tail;
- KB HTTP and client round-trip coverage for edge metadata;
- client fail-closed coverage for legacy-only blast responses that cannot prove edge metadata;
- successful `aimee-kb` and `aimee-blast-radius-eval` builds; and
- the checked-in deterministic corpus updated to make exact identities, provenance, freshness, and
  zero substring collisions the acceptance contract.

Repository-wide validation also passed all 39 lint checks and the complete local unit-test suite.
Tests that require live PostgreSQL or signed KMS fixtures reported their expected environment skips;
the protected CI jobs provide those services.

The standalone Postgres corpus binary was not run locally because this checkout has no
`AIMEE_DB2_EVAL_URL` or local Postgres client. CI owns that disposable-Postgres execution; the same
resolver is exercised locally through the repository's SQLite PostgreSQL shim.
