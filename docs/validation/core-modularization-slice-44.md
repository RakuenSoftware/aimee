# Core modularization slice 44: declare workspace ownership

## Scope

This slice declares the `workspace` descriptor's `sources`, `private_headers`, `tests`, and `docs`
fields. It does not set `ownership_complete`. It is the declaration half of the declaration-then-latch
pair; the latch, its mutation coverage, and the completeness audit follow in slice 45. It changes
descriptor metadata, the regenerated test-registration baseline, documentation, and cleanup accounting
only; no production code, public symbol, build membership, configuration, storage, or runtime behavior
changes. No header is moved and no include site is rewritten.

`workspace` is the third Class B module, after governance and learning, and the largest declared so
far: eleven sources, eleven module-root headers, and eleven direct tests carved out of a
twelve-target `unit-test-workspace*` family.

## What the module owns

The module root `src/modules/workspace/` contains, excluding `module.yaml`:

- Eleven sources: `cli_workspace_serve.c`, `workspace.c`, `workspace_handle.c`,
  `workspace_manifest.c`, `workspace_mirror.c`, `workspace_provider_container.c`,
  `workspace_provider_detached.c`, `workspace_runner_queue.c`, `workspace_runner_registry.c`,
  `workspace_scope.c`, `workspace_turn.c`.
- Eleven headers: the paired header for each source except `cli_workspace_serve.c`, plus
  `workspace_provider.h`, the provider dispatch interface, which has no paired source.

No `src/modules/workspace/include/aimee/workspace/` directory exists, so every header is at the module
root and is declared in `private_headers`. The header-layout checker constrains only declared
`public_headers`, so the private declaration is accurate to the layout; any future promotion of a
workspace header to the canonical include tree is a separate header-layout slice.

## Source liveness

Every declared source is live:

- `workspace.c`, `workspace_handle.c`, `workspace_mirror.c`, `workspace_scope.c`, and
  `workspace_turn.c` have large external caller counts across the server, posix, db2, and cmd layers
  (workspace resolution, handles, mirroring, scope containment, and per-turn binding are used
  throughout agent execution).
- `workspace_manifest.c` and `workspace_runner_registry.c` have external callers in the runner and
  serve paths.
- `cli_workspace_serve.c` is the client serve entry point.
- `workspace_provider_container.c`, `workspace_provider_detached.c`, and `workspace_runner_queue.c`
  have no external includer but are live module-internal units. The container and detached providers
  are provider implementations selected through `workspace_provider.h`, the module's own dispatch
  interface: `workspace_turn.c` includes both and holds a per-turn container provider, and
  `cli_workspace_serve.c` includes the detached provider. The runner queue is consumed through
  `workspace_runner_registry.h`, the module's runner surface. Their externally visible boundary is the
  interface header; the implementation source sitting behind it with no outside includer is the
  expected shape for a module-internal unit, not a dead island.

## Build membership

Make's `DATA_SRCS` compiles all eleven sources and carries the `-Imodules/workspace` include path.
CMake compiles four: `cli_workspace_serve.c`, `workspace.c`, `workspace_manifest.c`, and
`workspace_provider_detached.c`, the entry, the core, the manifest, and the one provider the thin
`aimee` client instantiates when it serves a detached workspace. It omits the seven server/runner-side
units: `workspace_handle.c`, `workspace_mirror.c`, `workspace_provider_container.c`,
`workspace_runner_queue.c`, `workspace_runner_registry.c`, `workspace_scope.c`, and
`workspace_turn.c`. This is the same intentional thin-client profile boundary recorded for gateway
(slice 38), audit (slice 34), and learning (slice 42), not source-list drift: the four CMake sources
are the client-serving subset, and the required Windows and Linux CMake jobs build the thin client
green from exactly that four-source set, which is the standing evidence that the client's link-time
closure does not reach the omitted seven. The descriptor records canonical source ownership, which both
build systems agree on; it does not claim identical build-product membership.

## Test membership

There are twelve `test_workspace*.c` files and twelve `unit-test-workspace*` Make targets. Eleven are
declared, one per module concern: `test_workspace.c`, `test_workspace_handle.c`,
`test_workspace_manifest.c`, `test_workspace_mirror.c`, `test_workspace_provider.c`,
`test_workspace_provider_container.c`, `test_workspace_provider_detached.c`,
`test_workspace_runner_queue.c`, `test_workspace_runner_registry.c`, `test_workspace_scope.c`, and
`test_workspace_turn.c`.

The twelfth, `test_workspace_memory.c`, is not a workspace test. Its subject is
`memory_auto_tag_workspace`, which is defined in `src/modules/memory/memory_core.c`; it exercises
memory's workspace-scoped tagging through the DB2 test shim and links `workspace.o` only to supply
workspace identity. It carries the `workspace` name but is a memory test, the same situation as
learning's KB test `test_learning_synth.c` and gateway's delivery-binary `test_gateway_*` tests. It
stays unclaimed and will belong to memory's eventual ownership.

CTest registers none of the workspace tests, consistent with the module's server/runner sources sitting
outside the thin-client profile. `scripts/check_module_test_registration.py` now records eleven
workspace rows (`make: true`, `ctest: false`); that regeneration is the only reason the baseline file
changes.

## Why declare without latching

The latch asserts the descriptor exhaustively covers the module root. That is true today. The module
root holds exactly these eleven sources and eleven headers, so the latch would pass. It is deferred
because declaring the files and asserting completeness are distinct claims, and the roundtable required
the completeness audit to review declarations merged on their own first rather than authored in the
same change. The validator accepts a declared-but-unlatched descriptor: it checks each declared path
exists and resolves within the module, and enforces set-equality only when `ownership_complete` is
true.

## Regression controls

The declaration is covered by the existing descriptor validation: every declared path must exist and
resolve within the module, and the regenerated test-registration baseline pins the eleven workspace
tests' per-suite registration. The empty-domain guard from slice 39 does not apply, because the module
root is not empty. The latch mutation coverage, source removal, private-header removal, planted files,
cleared latch, is deferred to slice 45, where `ownership_complete` is set and those mutations become
meaningful.

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
```

The eleven workspace unit tests build under Make from the module objects; the required pull-request
CMake jobs build the thin client from the four-source subset and are the standing evidence for the
thin-client boundary above.

Slice 45 sets `ownership_complete: true`, adds the workspace latch mutation tests, and records the
completeness audit. Technical-writer review, exact-final-diff roundtable approval, and every required
pull-request check are required before merge.
