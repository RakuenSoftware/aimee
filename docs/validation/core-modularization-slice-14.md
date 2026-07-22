# Core modularization slice 14: memory-family documentation

## Outcome

This slice promotes five related descriptors from documentation debt: required `memory`, `learning`,
`skills`, and `response-composition`, plus optional default-off `kb-synthesis`. Each document uses the
individual-module contract introduced in slice 13 and grounds ownership in current C sources, routes,
configuration, data, tests, and failure behavior.

The review confirms the intended boundary while making current placement debt explicit. Memory owns
code intelligence, embedding, reranking, recall, and assembly. Learning and skills are required parts of
the adaptive user experience. Response-composition owns the normal final-answer contract but has not yet
been physically consolidated under its descriptor directory. KB-synthesis owns heavyweight Tier-B
knowledge reasoning, not required Tier-A extraction/indexing or ordinary response composition.

## Deep-dive findings

- `src/modules/memory` already contains a large coherent implementation, while DB1/DB2, KB services,
  root commands, clients, and routes remain relocation candidates that require schema and surface care.
- `src/modules/learning` has a typed router implementation; its command, persistence, KB worker, and
  review surfaces remain distributed. Optional candidate synthesis does not make core learning optional.
- the `skills` descriptor and `src/modules/skill` implementation use different directory plurality. The
  cleanup target is one canonical plural module, not a forwarding layer or duplicate registry.
- `src/modules/response-composition` contains only `module.yaml`; canonical IR response construction and
  finalization are currently spread across IR, server, gateway, delegates, translation, and delivery.
- `src/modules/kb-synthesis` also contains only its descriptor. The existing curator tree mixes required
  ingestion/indexing with optional Tier-B reasoning. A later source-movement slice must split by function
  before relocation instead of moving the curator wholesale.

No feature is declared dead from a name-only search. The documents identify removal/consolidation rules,
and later implementation slices must pair definition/reference inventories with supported-journey tests.

## Cleanup and scope

This is documentation and baseline accounting only. It changes no production source, descriptor,
dependency, build graph, route, configuration behavior, database, GUI, or runtime profile. The status
baseline shrinks by exactly five entries; the remaining nineteen module documents stay explicit debt.

## Verification

- `python3 -I -S scripts/check_module_docs.py`
- `python3 -I scripts/tests/test_check_module_docs.py -v`
- `python3 -I -S scripts/check_module_source_ownership.py`
- `python3 -I -S scripts/check_cleanup_ledger.py`
- `python3 -I -S scripts/refactor_baselines.py`
- `make -C src lint`
- feature-branch pull-request CI
