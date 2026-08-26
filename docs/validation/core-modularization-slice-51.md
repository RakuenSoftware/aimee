# Core modularization slice 51: latch git ownership

## Scope

This slice sets `ownership_complete: true` on the `git` descriptor. It is the latch half of the
declaration-then-latch pair; slice 50 declared and audited the twenty-six sources, eighteen module-root
private headers, fourteen direct tests, and document on their own, and this slice asserts those
declarations exhaustively cover the module root and adds the latch mutation coverage. It changes
descriptor metadata, validation coverage, documentation, and cleanup accounting only; no production
code, public symbol, build membership, configuration, storage, or runtime behavior changes. Neither the
`git-core-contract` proposal nor its approval evidence is touched.

`git` is the fifteenth latched module and the sixth Class B module. Its twenty-six-element source set
and eighteen-element header set are both the largest latched so far.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/git/include/aimee/git/` (which does not exist). The module root contains exactly the
twenty-six declared sources and eighteen declared headers and nothing else matching those roles, so the
declared sets equal the actual sets and the latch is exact. The descriptor's `docs` field equals
`["docs/modules/git.md"]`, as the latch requires.

Slice 50 established the source liveness, build-membership, and test-classification evidence, and the
slice-decision roundtable approved the calls: all eighteen headers stay privately declared (two unpaired
seam/shared headers, `git_verify_internal.h` and `mcp_git.h`); the fourteen declared tests include three
CTest-only ones recorded `make: false, ctest: true`; `test_forge_app_token.c` (root-level source) and
`test_forge_credentials_live.c` (supplementary live harness) are not claimed; and the CMake
twelve-of-twenty-six membership is an intentional thin-client boundary. That audit is not repeated here;
this slice adds only the completeness assertion on the already-reviewed declarations.

Completeness is a file-ownership statement about the module root as it stands. It does not assert that
the root-level `src/forge_app_token.c`, the server/kb git-facing code, or any adjacent git-touching code
outside the module root has been moved in, and it does not claim identical build-product membership
across Make and CMake. The fourteen credential/OAuth/ops/forge-vault/host/org-repos/PR-API sources are
Make-only. It is also independent of the `git-core-contract` governance, which bounds git's core
capability rather than its file ownership.

## Regression controls

The descriptor mutation suite removes `git_ops.c` and requires `rule=ownership-complete` on `/sources`,
and removes `git_verify_internal.h` and requires the same rule on `/private_headers`. It plants
`src/modules/git/undeclared.c` and `undeclared.h` and requires the rule on `/sources` and
`/private_headers`. It removes `docs/modules/git.md` from the descriptor's `docs` field and requires the
rule on `/docs`. The graph-derived latched-descriptor assertion from slice 43 now also covers `git`, so
clearing the latch fails directly. The empty-domain guard from slice 39 does not apply, because the
module root is not empty. The unmodified descriptor graph must pass.

## Verification

Run from the repository root; each must exit zero:

```sh
python3 scripts/validate_module_descriptors.py --check-schema src/modules
python3 -m unittest scripts.tests.test_validate_module_descriptors
python3 scripts/check_module_docs.py
python3 scripts/check_cleanup_ledger.py
python3 scripts/check_module_test_registration.py
python3 scripts/check_module_source_ownership.py
python3 scripts/check_git_core_contract.py --require-status roundtable-approved
python3 scripts/check_module_header_layout.py
python3 scripts/refactor_baselines.py check
make -C src -j2
make -C src lint
make -C src -j2 build/obj/tests/unit-test-git-ops build/obj/tests/unit-test-mcp-git
src/build/obj/tests/unit-test-git-ops
src/build/obj/tests/unit-test-mcp-git
```

`cmake` is unavailable in the environment used for this slice; the three CTest-only git tests and the
twelve-source thin-client subset are covered by the required pull-request CMake jobs.

With `git` latched, fifteen modules carry `ownership_complete`. The remaining three Class B modules
(`delegates`, `workflows`, `memory`) follow the same declaration-then-latch pair, and the eight Class A
modules remain blocked by the empty-domain guard and tracked in
`docs/validation/core-modularization-class-a-migration.md`. Technical-writer review, exact-final-diff
roundtable approval, and every required pull-request check are required before merge.
