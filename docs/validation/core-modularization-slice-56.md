# Core modularization slice 56: declare memory ownership

## Scope

This slice declares the `memory` descriptor's `sources`, `private_headers`, `tests`, and `docs`
fields. It does not set `ownership_complete`. It is the declaration half of the declaration-then-latch
pair; the latch, its mutation coverage, and the completeness audit follow in slice 57. It changes
descriptor metadata, the regenerated test-registration baseline, validator-test fixtures,
documentation, and cleanup accounting only; no production code, public symbol, build membership,
configuration, storage, or runtime behavior changes. No header is moved and no include site is
rewritten.

`memory` is the ninth and final Class B module and the largest by source count. Latching it in slice
57 completes the Class B programme: all nine declared and latched, leaving only the eight Class A
modules, which are blocked on real code migration rather than on declaration work.

## What the module owns

The module root `src/modules/memory/` contains, excluding `module.yaml`, thirty-two sources and twelve
headers. No `src/modules/memory/include/aimee/memory/` directory exists, so every header is at the
module root and is declared in `private_headers`.

Five headers carry **no paired source**. The highest unpaired count in the module graph:
`memory_assemble_util.h` (pure string helpers), `memory_core_internal.h` (the core seam),
`memory_ontology.h` (the ontology vocabulary), `memory_platform.h` (the platform shim), and
`memory_rewrite_llm.h` (the rewrite-LLM contract). The other seven pair with a source; the remaining
twenty-five sources declare through those twelve headers.

Every source is live, spanning the memory core and its CRUD, search, tiers, helpers and scope-embed
family, assembly and context, advanced retrieval, conflict and effective resolution, directives,
episodes, extraction, the fact and PII gates, graph and graph-fusion, health, improve, lifecycle,
logic, maintenance, profile packs, prospective memory, the provider seam, redirect, scan, and the
gateway memory stage.

## Build membership

Make compiles all thirty-two sources. CMake compiles nineteen, omitting thirteen: `gw_stage_memory.c`,
the memory-core family beyond `memory_core.c` (`memory_core_crud.c`, `memory_core_helpers.c`,
`memory_core_helpers_b.c`, `memory_core_scope_embed.c`, `memory_core_search.c`,
`memory_core_search_b.c`, `memory_core_search_c.c`, `memory_core_tiers.c`),
`memory_extract_patterns.c`, `memory_fact_gate.c`, `memory_graph_fusion.c`, and `memory_pii_gate.c`,
the server/kb-side units. This is the same intentional thin-client profile boundary recorded for the
eight earlier Class B modules, evidenced by the green thin-client CMake jobs. The descriptor records
canonical source ownership, which both build systems agree on; it does not claim identical
build-product membership.

## The `memory` name collides in two directions

A `*memory*` filename is not an ownership signal for this module, and the collision is worse than the
`wfe_` case recorded in slice 54 because there are two colliding surfaces rather than one:

- **DB1 owns a separate working-memory store.** `src/db1/wm.c` is a DB1 source, tested by
  `test_working_memory.c`, which links `db1/wm.o`.
- **The root level owns the memory-interception harness.** `src/harness_memory_common.c`,
  `harness_memory_scope.c`, `harness_memory_audit.c`, and `harness_memory_spill.c` are root-level
  sources, tested by the four `test_harness_memory*.c` files.

Neither family is module-local, so neither is claimed. Any future `*memory*` test or source must be
attributed by where its subject lives, not by its name. This warning is recorded here at the roundtable's
direction, mirroring the `wfe_` warning in slice 54.

## Test membership

Twenty-three candidates. Classification is by linked subject where the target names specific objects,
and by the test's own stated subject where it links the shared `TEST_CORE_OBJS` bundle. That bundle
contains every memory source, so linkage alone cannot discriminate, the same situation as config.

Declared (sixteen). Eight name a memory object directly:

- `test_gw_stage_memory.c` → `gw_stage_memory.c`
- `test_memory.c` → the core (links `memory_advanced`, `memory_assemble`)
- `test_memory_fact_gate.c` → `memory_fact_gate.c`
- `test_memory_profiles.c` → `memory_profile_pack.c`
- `test_memory_provider.c` → `memory_provider.c`
- `test_memory_redirect.c` → `memory_redirect.c`
- `test_memory_retrieval_eval.c` → corpus-based memory retrieval (links `memory_advanced`,
  `memory_assemble`)
- `test_memory_assemble_util.c` → the pure string helpers declared in `memory_assemble_util.h`

Eight link the shared bundle and are classified by stated subject:

- `test_memory_advanced.c`, `test_memory_filter.c`, `test_memory_health.c`
- `test_memory_lanes.c`: two-lane retrieval
- `test_memory_recall_pivot.c`: per-turn topic-pivot detection
- `test_memory_ranker_boundary.c`: the Recall/Calibrate boundary enforced by `memory_ranker_input_t`,
  a memory type
- `test_memory_embed_dim_guard.c`: **a memory↔DB2 boundary test**, called out explicitly rather than
  left implicit. Its subject is that the memory/KB vector upsert must reject a vector whose dimension
  does not match the configured `db2_embedding_dim` instead of silently succeeding. The guard is on
  the memory upsert path while the dimension it checks is a DB2 configuration value, so linkage cannot
  decide it; the roundtable confirmed it belongs to memory because the behaviour under test is
  memory's refusal to write, not DB2's configuration.
- `test_workspace_memory.c`: claimed here because its subject `memory_auto_tag_workspace` is defined
  in `src/modules/memory/memory_core.c`. Slice 44 excluded it from `workspace` on exactly that basis;
  claiming it now closes the loop rather than leaving it orphaned between two modules.

Excluded (seven), each with its subject outside the module root: the four `test_harness_memory*.c`
files (root-level harness), `test_kb_client_memory.c` (links `modules/kb_client/*`),
`test_server_memory_benchmark.c` (links `server/server_memory_benchmark.o`), and
`test_working_memory.c` (links `db1/wm.o`).

Two of the sixteen (`test_memory_provider.c` and `test_memory_redirect.c`) are CTest-registered as
well as Make-registered; the other fourteen run under Make alone.
`scripts/check_module_test_registration.py` now records sixteen memory rows with those exact flags;
that regeneration is the only reason the baseline file changes.

## A stale validator fixture, repaired

`test_ownership_is_optional_and_report_preserves_declared_order` asserted that the `memory` descriptor
reports every ownership role as empty. It was the programme's chosen example of an undeclared module.
Declaring `memory` made that fixture false. Rather than repoint it at another named module, which
would go stale again the moment that module is declared, it now derives the undeclared set from the
ownership report and asserts the empty-role property across all of them. That keeps the coverage and
removes the recurring maintenance.

## Why declare without latching

The latch asserts the descriptor exhaustively covers the module root. That is true today. The module
root holds exactly these thirty-two sources and twelve headers, so the latch would pass. It is
deferred because declaring the files and asserting completeness are distinct claims, and the roundtable
required the completeness audit to review declarations merged on their own first rather than authored
in the same change.

## Regression controls

The declaration is covered by the existing descriptor validation: every declared path must exist and
resolve within the module, and the regenerated test-registration baseline pins the sixteen memory
tests' per-suite registration. The empty-domain guard from slice 39 does not apply, because the module
root is not empty. The latch mutation coverage, source removal, private-header removal, planted files,
cleared latch, is deferred to slice 57.

## Verification

Run from the repository root; each must exit zero:

```sh
python3 scripts/validate_module_descriptors.py --check-schema src/modules
python3 -m unittest scripts.tests.test_validate_module_descriptors
python3 scripts/check_module_docs.py
python3 scripts/check_cleanup_ledger.py
python3 scripts/check_module_test_registration.py
python3 -m unittest scripts.tests.test_check_module_test_registration
python3 scripts/check_module_source_ownership.py
python3 scripts/check_module_header_layout.py
python3 scripts/refactor_baselines.py check
make -C src -j2
make -C src lint
make -C src -j2 build/obj/tests/unit-test-memory-provider build/obj/tests/unit-test-memory-redirect
src/build/obj/tests/unit-test-memory-provider
src/build/obj/tests/unit-test-memory-redirect
```

Slice 57 sets `ownership_complete: true`, adds the memory latch mutation tests, and records the
completeness audit, completing the Class B programme. Technical-writer review, exact-final-diff
roundtable approval, and every required pull-request check are required before merge.
