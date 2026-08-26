# Core modularization slice 54: declare workflows ownership

## Scope

This slice declares the `workflows` descriptor's `sources`, `private_headers`, `tests`, and `docs`
fields. It does not set `ownership_complete`. It is the declaration half of the declaration-then-latch
pair; the latch, its mutation coverage, and the completeness audit follow in slice 55. It changes
descriptor metadata, the regenerated test-registration baseline, documentation, and cleanup accounting
only; no production code, public symbol, build membership, configuration, storage, or runtime behavior
changes. No header is moved and no include site is rewritten.

`workflows` is the eighth Class B module and the second-largest: thirty sources, twenty-four
module-root headers, and thirty-three direct tests carved out of thirty-seven candidates. It is also
the first optional (`enabled_by_default: false`) Class B module since plugin-loader.

## What the module owns

The module root `src/modules/workflows/` contains, excluding `module.yaml`, thirty sources and
twenty-four headers. No `src/modules/workflows/include/aimee/workflows/` directory exists, so every
header is at the module root and is declared in `private_headers`.

Every one of the twenty-four headers pairs with a like-named source, unlike config, vault, git, and
workspace, this module has no unpaired seam header. Six sources carry no paired header:
`wfe_canonical.c`, `wfe_custom.c`, `wfe_live_forge.c`, `wfe_router_catalog.c`, `wfe_scheduler.c`, and
`wfe_validate.c`; they declare through the paired headers.

Every source is live, spanning the definition/validation front end, the engine and block executors,
advance and approval, autonomy and the native gate, routing and the router catalog, the scheduler,
delivery and externalization, enforcement, the roundtable and panel seams, replay-worktree, the live
providers, and the gateway orchestration entry point.

## Build membership

Make compiles all thirty sources. CMake compiles twenty-four, omitting six: `gw_orch_workflows.c`,
`wfe_live_foreach.c`, `wfe_live_forge.c`, `wfe_live_panel.c`, `wfe_panel_roundtable.c`, and
`wfe_replay_worktree.c`. The live, panel, forge, replay, and gateway-orchestration units that are
server-side. This is the same intentional thin-client profile boundary recorded for gateway (slice 38),
audit (slice 34), learning (slice 42), workspace (slice 44), vault (slice 46), config (slice 48), git
(slice 50), and delegates (slice 52), evidenced by the green thin-client CMake jobs. The descriptor
records canonical source ownership, which both build systems agree on; it does not claim identical
build-product membership.

## Test membership, and a name-prefix trap

**The `wfe_` prefix is shared with DB1.** `src/db1/wfe_store.c` and `src/db1/wfe_binding.c` are DB1
sources, so a `test_wfe_*` filename says nothing about which module owns the test. This module is the
clearest case in the program where name-based classification would go wrong, and three of the four
exclusions below exist precisely because of it. Any future `test_wfe_*.c` must be classified by which
object its target links, not by its name.

Classification is by linked subject, read from `src/tests/Rules.mk`. Thirty-three of the thirty-seven
candidates link at least one `modules/workflows/*.o` and are declared: the engine core
(`test_workflow.c`, `test_wfe_engine.c`, `test_wfe_blocks.c`, `test_wfe_custom.c`,
`test_wfe_sliced_build.c`), advance and approval (`test_wfe_advance.c`, `test_wfe_advance_exec.c`,
`test_wfe_approval.c`, `test_wfe_gate_reject.c`), autonomy and routing (`test_wfe_autonomy.c`,
`test_wfe_autonomous_route.c`, `test_wfe_router.c`, `test_wfe_router_catalog.c`), the manager and
foreach families (`test_wfe_manager_artifacts.c`, `test_wfe_manager_blocks.c`,
`test_wfe_manager_flow.c`, `test_wfe_foreach.c`, `test_wfe_foreach_spawn.c`), binding and block
resolution (`test_wfe_bind_ingress.c`, `test_wfe_block_resolve.c`), enforcement and externalization
(`test_wfe_enforce.c`, `test_wfe_externalization.c`, `test_wfe_native_gate.c`), delivery and safety
(`test_wfe_deliver.c`, `test_wfe_safety.c`, `test_wfe_failure_taxonomy.c`), the seams
(`test_wfe_roundtable.c`, `test_wfe_panel_roundtable.c`, `test_wfe_delegate_seam.c`), the scheduler
and replay (`test_wfe_scheduler.c`, `test_wfe_replay_worktree.c`), the web API
(`test_wfe_webapi.c`), and `test_gw_orch_workflows.c` → `gw_orch_workflows.c`.

`test_wfe_webapi.c` is the most cross-cutting of the thirty-three: it links `db1/wfe_store.o`,
`modules/config/*`, and `server/*` objects alongside three workflows engine objects
(`wfe_canonical`, `wfe_custom`, `wfe_def`). It is included because it links workflows objects and its
subject is the workflow engine's web API surface, the engine's external HTTP contract. Cross-cutting
has not been an exclusion criterion in any of the eight prior modules, and inventing one here would
orphan the engine's HTTP boundary.

Four candidates are excluded because they link **no** workflows object at all:

- `test_wfe_binding.c` links only `db1/*` objects and exercises re-bind, single-writer conflict,
  unbind, and reclaim, the DB1 binding store.
- `test_wfe_gate_apply.c` links `db1/wfe_store.o` and exercises state-precondition guarantees behind
  operator human-gate decisions, the DB1 store.
- `test_wfe_submitter.c` links `db1/wfe_store.o` and exercises `db1_work_item_submit_capped` and
  per-principal count helpers, the DB1 store.
- `test_workflow_gate_caps.c` links no module object at all; it asserts a route/capability contract
  (`POST /v1/workflow/items/<id>/gate`, `CAP_WORKFLOW_ADMIN`).

All thirty-three declared tests are Make-registered; eleven are additionally registered as CTest
cases (`test_wfe_advance`, `test_wfe_autonomous_route`, `test_wfe_deliver`, `test_wfe_enforce`,
`test_wfe_externalization`, `test_wfe_foreach`, `test_wfe_manager_artifacts`,
`test_wfe_manager_blocks`, `test_wfe_router`, `test_wfe_router_catalog`, `test_wfe_sliced_build`).
`scripts/check_module_test_registration.py` now records thirty-three workflows rows with those exact
per-suite flags; that regeneration is the only reason the baseline file changes, and it is the drift
detector that will fail CI if any declared test's registration moves before slice 55 latches.

## Why declare without latching

The latch asserts the descriptor exhaustively covers the module root. That is true today. The module
root holds exactly these thirty sources and twenty-four headers, so the latch would pass. It is
deferred because declaring the files and asserting completeness are distinct claims, and the roundtable
required the completeness audit to review declarations merged on their own first rather than authored
in the same change.

## Regression controls

The declaration is covered by the existing descriptor validation: every declared path must exist and
resolve within the module, and the regenerated test-registration baseline pins the thirty-three
workflows tests' per-suite registration. The empty-domain guard from slice 39 does not apply, because
the module root is not empty. The latch mutation coverage, source removal, private-header removal,
planted files, cleared latch, is deferred to slice 55.

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
make -C src -j2 build/obj/tests/unit-test-wfe-engine build/obj/tests/unit-test-wfe-router
src/build/obj/tests/unit-test-wfe-engine
src/build/obj/tests/unit-test-wfe-router
```

The eleven CTest-registered workflows tests are covered by the required pull-request CMake jobs, which
also build the thin client from the twenty-four-source subset.

Slice 55 sets `ownership_complete: true`, adds the workflows latch mutation tests, and records the
completeness audit. Technical-writer review, exact-final-diff roundtable approval, and every required
pull-request check are required before merge.
