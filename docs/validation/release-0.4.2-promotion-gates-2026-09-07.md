# 0.4.2 promotion gate repair

PR [#2967](https://github.com/RakuenSoftware/aimee/pull/2967) promotes the bundled
application from `testing` to `main`. Its failing checks required independently
published source mirrors and did not admit the already merged Go memory migration.
Later steps also exposed an incomplete module-documentation catalog. This repair
updates release metadata, checks, and documentation; it changes no runtime implementation.

## Application source provenance

`dependencies/aimee-application-sources.lock.json` freezes the core and all 35
canonical modules using the exporter's source-digest calculation. It records
bundled versus external ownership, classifications, placements, execution modes,
and process identities/grants. `c-repository-pins` verifies this snapshot on main
and promotion PRs while continuing local standalone export/build checks.

The external config module must still match its existing exact repository pin,
Go dependency version, and source digest. Snapshot generation refuses a stale
or duplicate external pin. The historical independent repository lock and
`src/core/VERSION` are unchanged. No module repository creation or independent
module publication is performed or required by this application-release workflow.
The original independent repository checker remains available for separately
requested module releases. See [repository extraction](../core/repository-extraction.md).

## DB2 migration review

The previous main contract has fingerprint
`bc88c720134efbd3b18a737d5c6bba252a59fcbb1568bbae1ca6bebfae8bfb75`.
Only comparison against that validated contract admits the exact transition
introduced by Go memory migration `3d48beb23b0d9f28a7b637d35c24df0a38e6ad3c`:

- Eighteen named C memory translation units retired from DB2.
- Three named node-kind/PII support units retired with their C policy implementations.
- Nine exact symbol-to-consumer mappings now supplied outside the DB2 object set.
- One additional system `memchr` reference from `fact_recall.c`.
- cJSON's recorded caller provenance shrinks only by the retired source paths.

The nine symbols resolve in the existing memory integration adapters:

| Symbols | Provider under `src/modules/memory/` |
| --- | --- |
| `db2_memory_provenance_by_id` | `memory_data_bus.c` |
| `db2_memory_scene_members`, `db2_memory_scenes_list_recent`, `memory_ontology_node_kind_to_text` | `memory_domain_bus.c` |
| `db2_memory_scope_bind_current`, `db2_memory_scope_context_get` | `memory_scope_connection.c` |
| `memory_pii_rel_sensitivity`, `memory_pii_turn_requests_sensitive` | `memory_pii_gate.c` |
| `pgvec_memory_vector_search_record_type` | `memory_domain_runtime_bus.c` |

All nine remain classified as injected module-contract debt. The real relocatable
link probe still records **149 unresolved symbols: 140 system imports and nine
module-contract imports**. This does not claim standalone DB2 runtime closure;
the process-activation gate remains enforced. The source-boundary and memory-C
checks remain enforced too. Other bases, extra source/support removals, additional
imports or consumers, and incorrect import classifications fail the ratchet.

## Release surface and documentation

The [public-surface diff](release-0.4.2-promotion-gates-2026-09-07/public-surface-diff.json)
records five added, one removed, and 22 changed public headers, plus the CLI,
route, configuration, schema, package, and public-symbol changes since main.
`tests/baselines/refactor/index.json` now freezes that release surface.

The documentation catalog now includes the existing Server, KB, and Providers
modules. Their guides and the existing memory guide use the required sections.
PostgreSQL and routing additions are placed within those sections and their
listed dependencies match the descriptors. PostgreSQL now documents its required
process and optional LUKS storage separately; memory documents the already
qualified personal semantic recall while retaining the KB-only reminder/directive
limitation. All 35 module guides pass without adding documentation-debt exceptions.

## Verification

The [module-inventory results](release-0.4.2-promotion-gates-2026-09-07/module-inventory-checks.json)
record the locally executed gates and suites, including the real DB2 probe,
activation refusal, module boundaries, generated contracts, release baseline,
documentation, and test registration. Both previous-ref comparisons also pass
against `origin/main`.

Additional checks passed: all 77 lint checks; 71 DB2 closure tests including nine
new migration regressions; seven application-lock tests with mutation subcases;
13 module-doc tests; 24 documentation-contract tests; the fixture-export pin
preservation regression; and actionlint on the changed workflow. The build-integrity assertion now verifies
this application gate; mutation fixtures reject a missing main guard, restored
independent-publication requirement, and integration lint/verify placement. Snapshot drift
and external-pin mutations fail without requiring any independent publication.

The unchanged runtime implementation was already exercised using published
`testing-a8abc2e` images on fresh VM 9437 on .253. The
[full qualification report](release-0.4.2-testing-a8abc2e-2026-09-07.md) and its
sanitized evidence accompany this repair. That qualification covers ordinary
storage, explicit LUKS, personal/shared memories, models, browser workflows,
persistence, upgrade, rollback, and recovery on Linux. Windows Docker Desktop
was not exercised. GitHub must still run the updated promotion checks and the
versioned release preflight; protected release approval and merging main are
separate from this repair.
