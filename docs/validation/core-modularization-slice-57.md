# Core modularization slice 57: latch memory ownership: Class B complete

## Scope

This slice sets `ownership_complete: true` on the `memory` descriptor. It is the latch half of the
declaration-then-latch pair; slice 56 declared and audited the thirty-two sources, twelve module-root
private headers, sixteen direct tests, and document on their own, and this slice asserts those
declarations exhaustively cover the module root and adds the latch mutation coverage. It changes
descriptor metadata, validation coverage, documentation, and cleanup accounting only; no production
code, public symbol, build membership, configuration, storage, or runtime behavior changes.

`memory` is the eighteenth latched module and the ninth and final Class B module. **With this slice the
Class B programme is complete**: every descriptor whose module root contained undeclared
implementation files now declares that root exhaustively and latches it.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/memory/include/aimee/memory/` (which does not exist). The module root contains exactly
the thirty-two declared sources and twelve declared headers and nothing else matching those roles, so
the declared sets equal the actual sets and the latch is exact. The descriptor's `docs` field equals
`["docs/modules/memory.md"]`, as the latch requires.

Slice 56 established the source liveness, build-membership, and test-classification evidence, and the
slice-decision roundtable approved the calls including the two boundary cases
(`test_memory_embed_dim_guard.c` as a memory-to-DB2 boundary test, and `test_workspace_memory.c`
claimed here to close the loop slice 44 opened). That audit is not repeated here; this slice adds only
the completeness assertion on the already-reviewed declarations.

Completeness is a file-ownership statement about the module root as it stands. It does not assert that
DB1's working-memory store (`src/db1/wm.c`), the root-level memory-interception harness
(`src/harness_memory_*.c`), or the server and kb memory-facing code have been moved in, and it does not
claim identical build-product membership across Make and CMake, where thirteen sources are Make-only.

## Where the programme now stands

Eighteen of the twenty-six module descriptors carry `ownership_complete`. The eight that do not are
exactly the Class A set, `benchmarks`, `control-web`, `execution-policy`, `kb-synthesis`,
`response-composition`, `routing`, `runtime-web`, `tools`, whose module roots hold nothing but
`module.yaml`. They are blocked by the `ownership-empty-domain` guard added in slice 39, which refuses
a latch on an empty root because set equality would hold vacuously. Their remaining work is migrating
real implementation under `src/modules/<id>/`, not declaration work, and it is tracked in
`docs/validation/core-modularization-class-a-migration.md`.

That means the declaration-and-latch phase of this programme is finished. What is left is code
migration for Class A, plus the deferred follow-ups this programme recorded rather than silently
absorbed: the API/ABI cleanup candidates in plugin-loader, module-runtime, and gateway; the orphan
`test_vault_custody_pkcs11.c`; the CTest registration of `test_plugin`; and the header-layout
promotions (`learning.h`, `config_fields.h`) that ownership slices were forbidden from performing.

## Regression controls

The descriptor mutation suite removes `memory_core.c` and requires `rule=ownership-complete` on
`/sources`, and removes `memory_core_internal.h` (one of the five unpaired headers) and requires the
same rule on `/private_headers`. It plants `src/modules/memory/undeclared.c` and `undeclared.h` and
requires the rule on `/sources` and `/private_headers`. It removes `docs/modules/memory.md` from the
descriptor's `docs` field and requires the rule on `/docs`. The graph-derived latched-descriptor
assertion from slice 43 now also covers `memory`, so clearing the latch fails directly. The
empty-domain guard from slice 39 does not apply, because the module root is not empty. The unmodified
descriptor graph must pass.

The ownership-report fixture repaired in slice 56 remains meaningful after this latch: it derives the
undeclared set from the report, and the eight Class A descriptors keep that set non-empty, so it still
asserts the empty-role property rather than passing vacuously.

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
make -C src -j2 build/obj/tests/unit-test-memory-provider build/obj/tests/unit-test-memory-redirect
src/build/obj/tests/unit-test-memory-provider
src/build/obj/tests/unit-test-memory-redirect
```

`cmake` is unavailable in the environment used for this slice; the two CTest-registered memory tests
and the nineteen-source thin-client subset are covered by the required pull-request CMake jobs.

Technical-writer review, exact-final-diff roundtable approval, and every required pull-request check
are required before merge.
