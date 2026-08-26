# Core modularization slice 41: latch governance ownership

## Scope

This slice sets `ownership_complete: true` on the `governance` descriptor. It is the latch half of the
declaration-then-latch pair; slice 40 declared and audited the descriptor's `sources`,
`private_headers`, `tests`, and `docs` fields on their own, and this slice asserts those declarations
exhaustively cover the module root and adds the latch mutation coverage. It changes descriptor
metadata, validation coverage, documentation, and cleanup accounting only; no production code, public
symbol, build membership, configuration, storage, or runtime behavior changes.

`governance` is the first latched module in the series to carry a private header. The nine latched by
slices 29-38 declared none, so this is the first time `validate_complete_ownership` enforces
set-equality on the `private_headers` role against a real module.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/governance/include/aimee/governance/` (which does not exist; the module has no public
header directory). The module root contains exactly `gw_stage_governance.c`, `gw_stage_governance.h`,
and `module.yaml`. The declared source set is `{gw_stage_governance.c}` and the declared
private-header set is `{gw_stage_governance.h}`; both equal their actual sets, so the latch is exact.
The descriptor's `docs` field equals `["docs/modules/governance.md"]`, as the latch requires.

Slice 40 established the liveness and build-membership evidence behind these declarations: the source
is compiled by Make in `DATA_SRCS`, has tracked production callers in `src/server/anthropic_http.c`
and `src/server/openai_chat.c`, and the private header is included by those two servers and the direct
test. That audit is not repeated here; this slice adds only the completeness assertion on top of the
already-reviewed declarations.

Completeness is a file-ownership statement about the module root as it stands. It does not assert that
the governance plane the canonical document describes, OIDC, organizational identity, policy
distribution, the console surfaces, has been migrated; that code remains distributed across the KB,
DB2, management, and console layers and is not module-local. The latch means the descriptor covers what
is under `src/modules/governance/` today, and nothing more is there today.

## Private-header enforcement, exercised for the first time

Until this slice, every latched module declared zero private headers, so the `private_headers` branch
of `validate_complete_ownership` only ever compared an empty actual set against an empty declared set.
`governance` gives it one real file on each side. The regression suite now removes
`gw_stage_governance.h` from the descriptor's `private_headers` and requires the validator to fail
`rule=ownership-complete` on `/private_headers` with the header named as missing, and it plants
`src/modules/governance/undeclared.h` and requires the same rule on the same pointer. This is the first
time the private-header half of the completeness domain is proven to bite on a declared module rather
than vacuously.

## Regression controls

The descriptor mutation suite removes `gw_stage_governance.c` and requires
`rule=ownership-complete` on `/sources`, and removes `gw_stage_governance.h` and requires the same rule
on `/private_headers`. It plants `src/modules/governance/undeclared.c` and `undeclared.h` and requires
the rule on `/sources` and `/private_headers` respectively. It removes `docs/modules/governance.md`
from the descriptor's `docs` field and requires the rule on `/docs`. The latched-descriptor assertion
introduced in slice 35 now also covers `governance`, so silently clearing the latch fails directly.
The empty-domain guard from slice 39 does not apply, because the module root is not empty. The
unmodified descriptor graph must pass.

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
make -C src -j2 build/obj/tests/unit-test-response-governance-stage
src/build/obj/tests/unit-test-response-governance-stage
```

`cmake` is unavailable in the environment used for this slice, and CMake compiles no governance source
under a target of its own, so there is no module-specific CMake command to add; the required
pull-request CMake jobs continue to cover the thin-client profile they own.

With `governance` latched, ten modules carry `ownership_complete`. The remaining eight Class B modules
follow the same declaration-then-latch pair, and the eight Class A modules remain blocked by the
empty-domain guard and tracked in `docs/validation/core-modularization-class-a-migration.md`.
Technical-writer review, exact-final-diff roundtable approval, and every required pull-request check
are required before merge.
