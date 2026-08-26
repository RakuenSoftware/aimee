# Core modularization slice 47: latch vault ownership

## Scope

This slice sets `ownership_complete: true` on the `vault` descriptor. It is the latch half of the
declaration-then-latch pair; slice 46 declared and audited the twelve sources, thirteen module-root
private headers, eleven direct tests, and document on their own, and this slice asserts those
declarations exhaustively cover the module root and adds the latch mutation coverage. It changes
descriptor metadata, validation coverage, documentation, and cleanup accounting only; no production
code, public symbol, build membership, configuration, storage, or runtime behavior changes.

`vault` is the thirteenth latched module. Its private-header set of thirteen is the largest in the
program, and it includes `vault_internal.h`, a header with no paired source, so the
`private_headers` set-equality check compares a thirteen-element declared set that is not merely the
mirror of the source set against the thirteen actual headers.

## Ownership domain

The completeness domain is every module-local file whose suffix matches the `sources` or
`private_headers` role, excluding `module.yaml` and everything under
`src/modules/vault/include/aimee/vault/` (which does not exist). The module root contains exactly the
twelve declared sources and thirteen declared headers and nothing else matching those roles, so the
declared sets equal the actual sets and the latch is exact. The descriptor's `docs` field equals
`["docs/modules/vault.md"]`, as the latch requires.

Slice 46 established the source liveness, build-membership, and test-classification evidence, and the
slice-decision roundtable approved the calls: all thirteen headers stay privately declared; the five
other-module `test_vault_*` files are not claimed; the orphan `test_vault_custody_pkcs11.c` stays
undeclared and flagged; and the `WITH_TPM2`-gated `test_vault_tpm2.c` is claimed as the only test of
the real tpm2 custody source. That audit is not repeated here; this slice adds only the completeness
assertion on the already-reviewed declarations.

Completeness is a file-ownership statement about the module root as it stands. It does not assert that
the server (`server_vault.c`, `server_vault_bootstrap.c`), KB (`kb_vault_policy.c`), or DB2 vault code
outside the module root has been moved in, and it does not claim identical build-product membership
across Make and CMake. The custody backends, `vault_hwm.c`, and `vault_server_key.c` are Make-only.

## Set-equality with an unpaired header

The `private_headers` declared set is thirteen entries against thirteen actual module-root headers,
the largest header set latched so far. Twelve of the thirteen mirror a source; the thirteenth,
`vault_internal.h`, is the backend-seam header with no paired source, so the header set is not simply
the source set with a suffix swap. The regression suite removes `vault_service.c` from `sources` and
requires `rule=ownership-complete` on `/sources` with the source named missing, and removes
`vault_internal.h` from `private_headers` and requires the same rule on `/private_headers` with the
header named missing, confirming the check bites on the unpaired header specifically, not only on a
header that shadows a removed source.

## Regression controls

The descriptor mutation suite removes `vault_service.c` and requires `rule=ownership-complete` on
`/sources`, and removes `vault_internal.h` and requires the same rule on `/private_headers`. It plants
`src/modules/vault/undeclared.c` and `undeclared.h` and requires the rule on `/sources` and
`/private_headers`. It removes `docs/modules/vault.md` from the descriptor's `docs` field and requires
the rule on `/docs`. The graph-derived latched-descriptor assertion from slice 43 now also covers
`vault`, so clearing the latch fails directly. The empty-domain guard from slice 39 does not apply,
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
make -C src -j2 build/obj/tests/unit-test-vault-service build/obj/tests/unit-test-vault-crypto
src/build/obj/tests/unit-test-vault-service
src/build/obj/tests/unit-test-vault-crypto
```

`test_vault_tpm2.c` builds only under `make WITH_TPM2=1` and is not in the default local test list.
`cmake` is unavailable in the environment used for this slice, and CMake compiles a subset of this
module's sources into the thin client without a module test target, so there is no module-specific
CMake command to add; the required pull-request CMake jobs continue to cover the thin-client profile
they own.

With `vault` latched, thirteen modules carry `ownership_complete`. The remaining five Class B modules
(`config`, `git`, `delegates`, `workflows`, `memory`) follow the same declaration-then-latch pair, and
the eight Class A modules remain blocked by the empty-domain guard and tracked in
`docs/validation/core-modularization-class-a-migration.md`. Technical-writer review, exact-final-diff
roundtable approval, and every required pull-request check are required before merge.
