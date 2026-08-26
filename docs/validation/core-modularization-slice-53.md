# Core modularization slice 53: latch delegates ownership

## Scope

This slice sets `ownership_complete: true` on the `delegates` descriptor. It is the latch half of the
declaration-then-latch pair; slice 52 completed the twenty-seven-source declaration and expanded the
declared tests to nineteen on their own, and this slice asserts those declarations exhaustively cover
the module root and adds the latch mutation coverage. It changes descriptor metadata, validation
coverage, documentation, and cleanup accounting only; no production code, public symbol, build
membership, configuration, storage, or runtime behavior changes.

`delegates` is the sixteenth latched module and the seventh Class B module. It is the first latched
module with **zero private headers and a canonical public header tree**, so it is the first whose
`private_headers` completeness role is enforced as an empty set rather than a declared list.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/delegates/include/aimee/delegates/`. The module root contains exactly the twenty-seven
declared sources and no headers at all. The declared source set equals the actual source set, and the
absent `private_headers` field is an empty declared set matching the empty actual set, so the latch is
exact on both roles. The descriptor's `docs` field equals `["docs/modules/delegates.md"]`, as the latch
requires.

Slice 52 established the source liveness, build-membership, and test-classification evidence, and the
slice-decision roundtable approved the calls: declare all twenty-seven sources, expand the tests to
nineteen, and omit the `private_headers` field entirely. That audit is not repeated here; this slice
adds only the completeness assertion on the already-reviewed declarations.

Completeness is a file-ownership statement about the module root as it stands. It does not assert that
the durable delegate worker and HTTP/RPC orchestration in `src/server/server_compute*`, or the
root-level `cmd_agent_delegate.c` entry point, have been moved in. The module document records those
as relocation debt, and it does not claim identical build-product membership across Make and CMake,
where four sources are Make-only.

## An absent role is enforced, not unchecked

Because `delegates` declares no `private_headers` field, the natural question is whether that role is
simply skipped. It is not. The mutation suite plants `src/modules/delegates/undeclared.h` and the
validator fails `rule=ownership-complete` on `/private_headers`, because the actual set becomes
non-empty while the declared set stays empty. The absent field therefore means *this role must remain
empty*, not *this role is unvalidated*: adding any module-root header to `delegates` fails CI until it
is either declared or moved under the canonical include tree. This was verified before the declaration
slice was written and is re-proven by the mutation added here.

## Regression controls

The descriptor mutation suite removes `delegate_driver.c` and requires `rule=ownership-complete` on
`/sources`. It plants `src/modules/delegates/undeclared.c` and `undeclared.h` and requires the rule on
`/sources` and `/private_headers` respectively, the latter being the empty-set enforcement described
above. It removes `docs/modules/delegates.md` from the descriptor's `docs` field and requires the rule
on `/docs`. The graph-derived latched-descriptor assertion from slice 43 now also covers `delegates`,
so clearing the latch fails directly. There is no declared-private-header removal case, because the
module declares none. The empty-domain guard from slice 39 does not apply, because the module root
holds twenty-seven sources. The unmodified descriptor graph must pass.

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
make -C src -j2 build/obj/tests/unit-test-delegate-driver build/obj/tests/unit-test-delegate-plan
src/build/obj/tests/unit-test-delegate-driver
src/build/obj/tests/unit-test-delegate-plan
```

`cmake` is unavailable in the environment used for this slice; the twenty-three-source thin-client
subset and the CTest registration of `test_delegate_plan` are covered by the required pull-request
CMake jobs.

With `delegates` latched, sixteen modules carry `ownership_complete`. The remaining two Class B
modules (`workflows`, `memory`) follow the same declaration-then-latch pair, and the eight Class A
modules remain blocked by the empty-domain guard and tracked in
`docs/validation/core-modularization-class-a-migration.md`. Technical-writer review, exact-final-diff
roundtable approval, and every required pull-request check are required before merge.
