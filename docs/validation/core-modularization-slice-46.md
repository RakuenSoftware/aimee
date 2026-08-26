# Core modularization slice 46: declare vault ownership

## Scope

This slice declares the `vault` descriptor's `sources`, `private_headers`, `tests`, and `docs`
fields. It does not set `ownership_complete`. It is the declaration half of the declaration-then-latch
pair; the latch, its mutation coverage, and the completeness audit follow in slice 47. It changes
descriptor metadata, the regenerated test-registration baseline, documentation, and cleanup accounting
only; no production code, public symbol, build membership, configuration, storage, or runtime behavior
changes. No header is moved and no include site is rewritten.

`vault` is the fourth Class B module and the most test-classification-heavy so far: twelve sources,
thirteen module-root headers, and seventeen `test_vault*.c` files across fifteen `unit-test-vault-*`
targets plus a gated harness and an orphan. Because it is security-sensitive, the source liveness and
test classification were audited individually and taken to the slice-decision roundtable.

## What the module owns

The module root `src/modules/vault/` contains, excluding `module.yaml`:

- Twelve sources: `vault_capability.c`, `vault_crypto.c`, `vault_custody_kms.c`,
  `vault_custody_mock.c`, `vault_custody_pkcs11.c`, `vault_custody_tpm2.c`, `vault_hwm.c`,
  `vault_kek_cache.c`, `vault_principal.c`, `vault_server_key.c`, `vault_service.c`,
  `vault_store.c`.
- Thirteen headers: the twelve paired headers plus `vault_internal.h`, the backend-seam header with
  no paired source.

No `src/modules/vault/include/aimee/vault/` directory exists, so every header is at the module root
and is declared in `private_headers`. The header-layout checker constrains only declared
`public_headers`, so the private declaration is accurate to the layout; any future promotion is a
separate header-layout slice.

## Source liveness

Every declared source is live. Ten have external callers across the server, kb, db2, and other module
layers, `vault_service.c` alone is reached by roughly thirty files. Two need explicit notes:

- `vault_hwm.c` has no external includer but is a live module-internal unit: `vault_custody_kms.c`
  includes `vault_hwm.h` and calls `vault_hwm_attest_verify` in its high-water-mark attestation path.
- The four custody backends are always compiled by Make. `WITH_TPM2` and `WITH_PKCS11` toggle a `-D`
  flag that switches `vault_custody_tpm2.c` and `vault_custody_pkcs11.c` between a real implementation
  and a fail-closed stub; the source is never omitted. `vault_custody_mock.c` is the always-available
  mock custody backend and `vault_custody_kms.c` the KMS backend.

## Build membership

Make's `DATA_SRCS` compiles all twelve sources and carries the `-Imodules/vault` include path. CMake
compiles six: `vault_capability.c`, `vault_crypto.c`, `vault_kek_cache.c`, `vault_principal.c`,
`vault_service.c`, `vault_store.c`, the units the thin `aimee` client reaches. It omits the four
custody backends, `vault_hwm.c`, and `vault_server_key.c`, which are the server/kb-side custody,
attestation, and server-sealed-KEK units. This is the same intentional thin-client profile boundary
recorded for gateway (slice 38), audit (slice 34), learning (slice 42), and workspace (slice 44), not
source-list drift. The descriptor records canonical source ownership, which both build systems agree
on; it does not claim identical build-product membership.

## Test membership

Classification is by subject, what a test exercises, per its header comment and the object it
primarily drives, not by the `vault` name. Eleven tests are declared:

- `test_vault_capability.c`, `test_vault_crypto.c`, `test_vault_kek_cache.c`,
  `test_vault_principal.c`, `test_vault_server_key.c`, `test_vault_service.c`, `test_vault_store.c`,
each drives its like-named vault source.
- `test_vault_kms.c` drives `vault_custody_kms.c` (includes `vault_custody_kms.h`) and transitively
  exercises `vault_hwm.c`.
- `test_vault_master_rotate.c` and `test_vault_seam.c` link only vault-module objects (master-key
  rotation over the service and server-key; the `vault_internal.h` backend seam over the store).
- `test_vault_tpm2.c` backs the `WITH_TPM2`-gated `p7-tpm2-harness` and is the only test that links
  and exercises the real `vault_custody_tpm2.c`. The slice-decision roundtable directed that it be
  claimed despite the gate, because omitting it would leave the descriptor claiming the tpm2 custody
  source with no named test, a hidden coverage gap. Its registration row records `make: true`
  (the harness object is referenced in `Rules.mk`) and `ctest: false`; the `WITH_TPM2` gate is an
  invocation condition, not a registration one.

Five `test_vault_*` files are excluded because their subject is another module's source, the same
subject-over-name criterion applied to gateway, learning, and workspace:

- `test_vault_audit.c` pins `vault_audit_server_write`, defined in `src/server/server_vault.c`, a
  server test.
- `test_vault_seal.c` drives the custody seal/unseal barrier; its primary include is
  `kb/kb_vault_policy.h` and it links `kb/kb_vault_policy.o` as its subject, a KB test.
- `test_vault_tpm2_stub.c` drives the no-libtss2 default-build seal path; its primary include is
  `kb/kb_vault_policy.h`, it links `kb_vault_policy.o`, and it does not link `vault_custody_tpm2.o`,
  so it exercises the barrier's stub behaviour rather than the vault custody source, a KB test.
- `test_vault_pg.c` is the Postgres credential-vault backend test and links `kb/kb_main.o`, a KB
  integration test.
- `test_vault_bootstrap.c` drives boot-time delegate-vault provisioning whose subject
  `server_vault_bootstrap.c` is a server file (it links `server/server_vault_bootstrap.o` alongside
  five vault objects), a server test.

`test_vault_custody_pkcs11.c` is referenced by no `Rules.mk` target: it is an orphan test file that no
build compiles or runs. An unbuilt file is not a coverage claim, so it is left undeclared and recorded
here as a cleanup follow-up (wire it to a target or delete it) rather than claimed.

`vault_hwm.c`, `vault_custody_mock.c`, and `vault_custody_pkcs11.c` have no declared direct test.
Tests are audited claims, not a per-source requirement: the high-water-mark path is covered
transitively through `test_vault_kms.c`, and the mock custody through the service and store suites.
The pkcs11 custody source's only test is the orphan above, which is why it has no named coverage until
that orphan is resolved.

CTest registers none of the vault tests, consistent with the module's server/kb-side sources sitting
outside the thin-client profile. `scripts/check_module_test_registration.py` now records eleven vault
rows (`make: true`, `ctest: false`); that regeneration is the only reason the baseline file changes.

## Why declare without latching

The latch asserts the descriptor exhaustively covers the module root. That is true today. The module
root holds exactly these twelve sources and thirteen headers, so the latch would pass. It is deferred
because declaring the files and asserting completeness are distinct claims, and the roundtable
required the completeness audit to review declarations merged on their own first rather than authored
in the same change. The validator accepts a declared-but-unlatched descriptor: it checks each declared
path exists and resolves within the module, and enforces set-equality only when `ownership_complete`
is true.

## Regression controls

The declaration is covered by the existing descriptor validation: every declared path must exist and
resolve within the module, and the regenerated test-registration baseline pins the eleven vault tests'
per-suite registration. The empty-domain guard from slice 39 does not apply, because the module root
is not empty. The latch mutation coverage, source removal, private-header removal, planted files,
cleared latch, is deferred to slice 47, where `ownership_complete` is set and those mutations become
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
make -C src -j2 build/obj/tests/unit-test-vault-service build/obj/tests/unit-test-vault-kms
src/build/obj/tests/unit-test-vault-service
src/build/obj/tests/unit-test-vault-kms
```

`test_vault_tpm2.c` builds only under `make WITH_TPM2=1`; its default build is the fail-closed stub
exercised elsewhere, so it is not in the default local test list above. The required pull-request CMake
jobs build the thin client from the six-source subset.

Slice 47 sets `ownership_complete: true`, adds the vault latch mutation tests, and records the
completeness audit. Technical-writer review, exact-final-diff roundtable approval, and every required
pull-request check are required before merge.
