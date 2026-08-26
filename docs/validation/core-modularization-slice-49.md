# Core modularization slice 49: latch config ownership

## Scope

This slice sets `ownership_complete: true` on the `config` descriptor. It is the latch half of the
declaration-then-latch pair; slice 48 declared and audited the fifteen sources, seven module-root
private headers, four direct tests, and document on their own, and this slice asserts those
declarations exhaustively cover the module root and adds the latch mutation coverage. It changes
descriptor metadata, validation coverage, documentation, and cleanup accounting only; no production
code, public symbol, build membership, configuration, storage, or runtime behavior changes.

`config` is the fourteenth latched module and the fifth Class B module. Its fifteen-element source set
is the largest latched so far.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/config/include/aimee/config/` (which does not exist). The module root contains exactly the
fifteen declared sources and seven declared headers and nothing else matching those roles, so the
declared sets equal the actual sets and the latch is exact. The descriptor's `docs` field equals
`["docs/modules/config.md"]`, as the latch requires.

Slice 48 established the source liveness, build-membership, and test-classification evidence, and the
slice-decision roundtable approved the calls with no findings: all seven headers stay privately
declared (`config_fields.h` noted as a future public-header candidate); all four `test_config*` tests
are config-owned by subject; and the CMake twelve-of-fifteen membership is an intentional thin-client
boundary. That audit is not repeated here; this slice adds only the completeness assertion on the
already-reviewed declarations.

Completeness is a file-ownership statement about the module root as it stands. It does not assert that
the cmd/server/UI config-facing code outside the module root has been moved in, and it does not claim
identical build-product membership across Make and CMake, `config_fields.c`, `config_mode.c`, and
`config_server_api.c` are Make-only.

## Regression controls

The descriptor mutation suite removes `config.c` and requires `rule=ownership-complete` on `/sources`,
and removes `config_internal.h` and requires the same rule on `/private_headers`. It plants
`src/modules/config/undeclared.c` and `undeclared.h` and requires the rule on `/sources` and
`/private_headers`. It removes `docs/modules/config.md` from the descriptor's `docs` field and requires
the rule on `/docs`. The graph-derived latched-descriptor assertion from slice 43 now also covers
`config`, so clearing the latch fails directly. The empty-domain guard from slice 39 does not apply,
because the module root is not empty. The unmodified descriptor graph must pass.

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
make -C src -j2 build/obj/tests/unit-test-config build/obj/tests/unit-test-config-snapshot
src/build/obj/tests/unit-test-config
src/build/obj/tests/unit-test-config-snapshot
```

`cmake` is unavailable in the environment used for this slice, and CMake compiles a twelve-source
subset of this module into the thin client, registering only `test_config` as a CTest case, so there is
no additional module-specific CMake command to add; the required pull-request CMake jobs continue to
cover the thin-client profile they own.

With `config` latched, fourteen modules carry `ownership_complete`. The remaining four Class B modules
(`git`, `delegates`, `workflows`, `memory`) follow the same declaration-then-latch pair, and the eight
Class A modules remain blocked by the empty-domain guard and tracked in
`docs/validation/core-modularization-class-a-migration.md`. Technical-writer review, exact-final-diff
roundtable approval, and every required pull-request check are required before merge.
