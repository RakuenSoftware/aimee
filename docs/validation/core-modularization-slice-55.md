# Core modularization slice 55: latch workflows ownership

## Scope

This slice sets `ownership_complete: true` on the `workflows` descriptor. It is the latch half of the
declaration-then-latch pair; slice 54 declared and audited the thirty sources, twenty-four module-root
private headers, thirty-three direct tests, and document on their own, and this slice asserts those
declarations exhaustively cover the module root and adds the latch mutation coverage. It changes
descriptor metadata, validation coverage, documentation, and cleanup accounting only; no production
code, public symbol, build membership, configuration, storage, or runtime behavior changes.

`workflows` is the seventeenth latched module and the eighth Class B module. Its thirty-source and
twenty-four-header declared sets are the second-largest in the program, behind git.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/workflows/include/aimee/workflows/` (which does not exist). The module root contains
exactly the thirty declared sources and twenty-four declared headers and nothing else matching those
roles, so the declared sets equal the actual sets and the latch is exact. The descriptor's `docs` field
equals `["docs/modules/workflows.md"]`, as the latch requires.

Slice 54 established the source liveness, build-membership, and test-classification evidence, and the
slice-decision roundtable approved the calls: all twenty-four headers stay privately declared; the
thirty-three declared tests exclude four that link no workflows object; and the CMake
twenty-four-of-thirty membership is an intentional thin-client boundary. That audit is not repeated
here; this slice adds only the completeness assertion on the already-reviewed declarations.

Completeness is a file-ownership statement about the module root as it stands. It does not assert that
DB1's `wfe_store.c` and `wfe_binding.c`, which share this module's `wfe_` name prefix but are DB1
sources, or the server-side workflow route handlers have been moved in, and it does not claim
identical build-product membership across Make and CMake, where six sources are Make-only.

## The `wfe_` prefix stays a live hazard after latching

Slice 54 recorded that `wfe_` is shared with `src/db1/wfe_store.c` and `src/db1/wfe_binding.c`, and
that three of its four test exclusions existed because of that collision. Latching does not resolve
the hazard, it narrows it: the completeness latch now guarantees that any new `wfe_*.c` or `wfe_*.h`
placed **in the module root** must be declared or CI fails, so a workflows-owned file cannot be added
silently. It says nothing about a new `wfe_`-named file added under `src/db1/`, and nothing about how
a future `test_wfe_*.c` is attributed. Test attribution remains a judgement made by linked object, and
the test-registration baseline is the mechanism that surfaces a change to any declared test's
registration.

## Regression controls

The descriptor mutation suite removes `wfe_engine.c` and requires `rule=ownership-complete` on
`/sources`, and removes `wfe_engine.h` and requires the same rule on `/private_headers`. It plants
`src/modules/workflows/undeclared.c` and `undeclared.h` and requires the rule on `/sources` and
`/private_headers`. It removes `docs/modules/workflows.md` from the descriptor's `docs` field and
requires the rule on `/docs`. The graph-derived latched-descriptor assertion from slice 43 now also
covers `workflows`, so clearing the latch fails directly. The empty-domain guard from slice 39 does not
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
make -C src -j2 build/obj/tests/unit-test-wfe-engine build/obj/tests/unit-test-wfe-router
src/build/obj/tests/unit-test-wfe-engine
src/build/obj/tests/unit-test-wfe-router
```

`cmake` is unavailable in the environment used for this slice; the eleven CTest-registered workflows
tests and the twenty-four-source thin-client subset are covered by the required pull-request CMake
jobs.

With `workflows` latched, seventeen modules carry `ownership_complete`. One Class B module remains (`memory`, the largest) and the eight Class A modules remain blocked by the empty-domain guard and
tracked in `docs/validation/core-modularization-class-a-migration.md`. Technical-writer review,
exact-final-diff roundtable approval, and every required pull-request check are required before merge.
