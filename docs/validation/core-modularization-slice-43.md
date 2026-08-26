# Core modularization slice 43: latch learning ownership

## Scope

This slice sets `ownership_complete: true` on the `learning` descriptor. It is the latch half of the
declaration-then-latch pair; slice 42 declared and audited the four sources, four module-root private
headers, two direct tests, and document on their own, and this slice asserts those declarations
exhaustively cover the module root and adds the latch mutation coverage. It changes descriptor
metadata, validation coverage, documentation, and cleanup accounting only; no production code, public
symbol, build membership, configuration, storage, or runtime behavior changes.

`learning` is the eleventh latched module and the second with declared private headers, after
governance. It is the first with more than one source and the first with more than one private header,
so it is the first module where the `sources` and `private_headers` set-equality checks each compare a
multi-element declared set against a multi-element actual set.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/learning/include/aimee/learning/` (which does not exist). The module root contains
exactly:

- Sources: `learning_bundle.c`, `learning_evidence.c`, `learning_implicit.c`, `learning_router.c`.
- Headers: `learning.h`, `learning_bundle.h`, `learning_evidence.h`, `learning_implicit.h`.

The declared source set equals the four actual sources and the declared private-header set equals the
four actual headers, so the latch is exact. The descriptor's `docs` field equals
`["docs/modules/learning.md"]`, as the latch requires.

Slice 42 established the liveness, build-membership, and test-classification evidence behind these
declarations, and the slice-decision roundtable approved the three scoping calls: `learning.h` stays
declared private with its de-facto-public status recorded and relocation deferred; the `learning_synth`
and `learning_version` tests are KB tests, not module tests; and the CMake omission of
`learning_bundle.c` is an intentional thin-client boundary. That audit is not repeated here; this slice
adds only the completeness assertion on the already-reviewed declarations.

Completeness is a file-ownership statement about the module root as it stands. It does not assert that
the DB2 persistence (`src/modules/db2/c/db2_learning.h`, `src/modules/db2/c/learning_synth_ops.c`) or the KB synthesis lane
have been moved into the module (they remain physical-ownership debt the document records) and it does
not resolve the `learning.h` layout question, which a separate header-layout slice owns.

## Multi-element set-equality, exercised for the first time

Every module latched before governance declared zero private headers, and governance declared exactly
one source and one header, so the `sources` and `private_headers` set-equality checks had only ever
compared sets of size zero or one. `learning` gives each check four declared entries against four actual
files. The regression suite removes `learning_router.c` from `sources` and requires
`rule=ownership-complete` on `/sources` with the source named missing, and removes `learning.h` from
`private_headers` and requires the same rule on `/private_headers` with the header named missing,
confirming the checks bite when one element of a multi-element declared set is dropped, not only when a
whole set is emptied.

## Regression controls

The descriptor mutation suite removes `learning_router.c` and requires `rule=ownership-complete` on
`/sources`, and removes `learning.h` and requires the same rule on `/private_headers`. It plants
`src/modules/learning/undeclared.c` and `undeclared.h` and requires the rule on `/sources` and
`/private_headers`. It removes `docs/modules/learning.md` from the descriptor's `docs` field and
requires the rule on `/docs`. The latched-descriptor assertion introduced in slice 35 is
converted here from a hardcoded module list to a graph-derived scan, so it now covers every latched
descriptor, clearing the latch on any of them fails directly. That list had silently stopped at
`gateway`: governance (slice 41) and learning were not added to it, so the assertion had quietly
stopped covering the two newest latched modules. Deriving the set from the descriptor graph fixes
governance retroactively and removes the drift for good; the scan guards only against a vacuous pass
(an empty glob) rather than a latched-module count, which would drift on every latch just as the
hardcoded list did. The empty-domain guard from slice 39 does not
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
make -C src -j2 build/obj/tests/unit-test-learning-bundle build/obj/tests/unit-test-learning-metrics
src/build/obj/tests/unit-test-learning-bundle
src/build/obj/tests/unit-test-learning-metrics
```

`cmake` is unavailable in the environment used for this slice, and CMake compiles a subset of this
module's sources into the thin client without a governance-style module test target, so there is no
module-specific CMake command to add; the required pull-request CMake jobs continue to cover the
thin-client profile they own.

With `learning` latched, eleven modules carry `ownership_complete`. The remaining seven Class B modules
follow the same declaration-then-latch pair, and the eight Class A modules remain blocked by the
empty-domain guard and tracked in `docs/validation/core-modularization-class-a-migration.md`.
Technical-writer review, exact-final-diff roundtable approval, and every required pull-request check are
required before merge.
