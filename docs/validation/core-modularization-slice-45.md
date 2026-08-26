# Core modularization slice 45: latch workspace ownership

## Scope

This slice sets `ownership_complete: true` on the `workspace` descriptor. It is the latch half of the
declaration-then-latch pair; slice 44 declared and audited the eleven sources, eleven module-root
private headers, eleven direct tests, and document on their own, and this slice asserts those
declarations exhaustively cover the module root and adds the latch mutation coverage. It changes
descriptor metadata, validation coverage, documentation, and cleanup accounting only; no production
code, public symbol, build membership, configuration, storage, or runtime behavior changes.

`workspace` is the twelfth latched module and the largest to date: eleven sources and eleven private
headers, so the `sources` and `private_headers` set-equality checks each compare an eleven-element
declared set against an eleven-element actual set.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/workspace/include/aimee/workspace/` (which does not exist). The module root contains
exactly the eleven declared sources and eleven declared headers and nothing else matching those roles,
so the declared sets equal the actual sets and the latch is exact. The descriptor's `docs` field
equals `["docs/modules/workspace.md"]`, as the latch requires.

Slice 44 established the liveness, build-membership, and test-classification evidence, and the
slice-decision roundtable approved the three scoping calls: all eleven headers stay privately declared;
`test_workspace_memory.c` is a memory test, not a module test; and the CMake four-of-eleven membership
is an intentional thin-client boundary. It also carried one item forward to this slice. Make the
`workspace_runner_queue.c` wiring explicit. `workspace_runner_queue.h` is included by
`workspace_runner_registry.h`, so the runner queue is part of the runner registry's compiled surface;
the registry has external callers in the runner and serve paths, so the queue is reachable in
production through it. That audit is otherwise not repeated here; this slice adds only the completeness
assertion on the already-reviewed declarations.

Completeness is a file-ownership statement about the module root as it stands. It does not assert that
the compatibility worktree declarations in `guardrails.h` (a documented consolidation seam) or any
adjacent workspace-touching code outside the module root have been moved in, and it does not claim
identical build-product membership across Make and CMake.

## Eleven-element set-equality

Governance latched with one source and one header; learning with four of each. `workspace` gives each
set-equality check eleven declared entries against eleven actual files, the largest in the program. The
regression suite removes `workspace_turn.c` from `sources` and requires `rule=ownership-complete` on
`/sources` with the source named missing, and removes `workspace_provider.h` from `private_headers`
and requires the same rule on `/private_headers` with the header named missing, confirming the checks
bite when one element of an eleven-element declared set is dropped.

## Regression controls

The descriptor mutation suite removes `workspace_turn.c` and requires `rule=ownership-complete` on
`/sources`, and removes `workspace_provider.h` and requires the same rule on `/private_headers`. It
plants `src/modules/workspace/undeclared.c` and `undeclared.h` and requires the rule on `/sources` and
`/private_headers`. It removes `docs/modules/workspace.md` from the descriptor's `docs` field and
requires the rule on `/docs`. The graph-derived latched-descriptor assertion from slice 43 now also
covers `workspace`, so clearing the latch fails directly. The empty-domain guard from slice 39 does not
apply, because the module root is not empty. The unmodified descriptor graph must pass.

## Verification

Run from the repository root; each must exit zero:

```sh
python3 scripts/validate_module_descriptors.py --check-schema src/modules
python3 -m unittest scripts.tests.test_validate_module_descriptors
python3 scripts/check_module_docs.py
python3 scripts/check_cleanup_ledger.py
python3 scripts/check_module_test_registration.py
python3 scripts/check_module_source_ownership.py
python3 scripts/check_module_header_layout.py
python3 scripts/refactor_baselines.py check
make -C src -j2
make -C src lint
make -C src -j2 build/obj/tests/unit-test-workspace build/obj/tests/unit-test-workspace-turn
src/build/obj/tests/unit-test-workspace
src/build/obj/tests/unit-test-workspace-turn
```

`cmake` is unavailable in the environment used for this slice, and CMake compiles a subset of this
module's sources into the thin client without a module test target, so there is no module-specific
CMake command to add; the required pull-request CMake jobs continue to cover the thin-client profile
they own.

With `workspace` latched, twelve modules carry `ownership_complete`. The remaining six Class B modules
(`vault`, `config`, `git`, `delegates`, `workflows`, `memory`) follow the same declaration-then-latch
pair, and the eight Class A modules remain blocked by the empty-domain guard and tracked in
`docs/validation/core-modularization-class-a-migration.md`. Technical-writer review, exact-final-diff
roundtable approval, and every required pull-request check are required before merge.
