# Core modularization slice 23: audit public-header ownership

## Scope

This slice starts from `30fcecac9fec842f072c94ab30e72ea6f8dd4a70` and canonicalizes the
required `audit` module's four public headers. It moves no implementation source and changes no
production logic, runtime behavior, storage, data, schema, configuration, or module boundary.
The descriptor declares exactly `audit_action.c`, `audit_ledger.c`, `audit_worm.c`, and
`audit_worm_chain.c` as existing implementation sources. Their only changes are canonical include
substitutions and formatter-required adjacent-comment alignment; public names and prototypes are
unchanged, as are Make/CMake source inputs and target visibility.

The physical moves are:

- `src/modules/audit/audit_action.h` to
  `src/modules/audit/include/aimee/audit/audit_action.h`
- `src/modules/audit/audit_ledger.h` to
  `src/modules/audit/include/aimee/audit/audit_ledger.h`
- `src/modules/audit/audit_worm.h` to
  `src/modules/audit/include/aimee/audit/audit_worm.h`
- `src/modules/audit/audit_worm_chain.h` to
  `src/modules/audit/include/aimee/audit/audit_worm_chain.h`

Every consumer updated in this slice uses `#include <aimee/audit/<header>.h>`; there are no
forwarding headers and no transition include root in the moved headers.

`audit_worm_chain.h` remains the engine-independent canonical hash/MAC boundary shared by the server
and KB stores. The server SQLite WORM store and KB PostgreSQL WORM store remain independently owned;
their storage, authority, checkpoint, seal, read, and failure guarantees do not change in this slice.

## Build and ownership boundary

Make uses the same spelling established by prior canonical modules:
`-Imodules/audit/include`. CMake defines
`AIMEE_AUDIT_INCLUDE_DIR` as `${AIMEE_SRC_DIR}/modules/audit/include` and substitutes that variable
for each former audit source-root include exposure. Existing target visibility is unchanged.

The descriptor declares the four audit implementations, four canonical public headers, and
`docs/modules/audit.md`. It records `tests: []` intentionally. Issue #1753 is the bounded follow-up
for a definition/caller, build, journey, store, and configuration audit of tests crossing server,
KB, vault, management, and storage boundaries. The empty list is machine-readable deferred
ownership, not absence of test coverage; the follow-up is due within the next two module re-rooting
slices.

## Reusable rejection rule

`scripts/check_module_header_layout.py` derives policy from every descriptor with declared public
headers. It rejects a same-basename shadow at the flat module root, quoted or angle-bracket basename
includes across repository C/C++ sources, and exact Make/CMake exposure of the module source root.
It also fails closed on missing canonical headers, mandatory build inputs, and source symlinks.
Diagnostics are sorted and deduplicated. This keeps migration residue separate from
implementation-source ownership and gives later header moves the same protection without a
per-module registry.

## Completed local verification

- `python3 -I -S scripts/check_module_header_layout.py`
- `python3 -I scripts/tests/test_check_module_header_layout.py -v`
- `python3 -I -S scripts/validate_module_descriptors.py --check-schema src/modules`
- `python3 -I -S scripts/validate_module_descriptors.py --emit-ownership src/modules`
- `python3 -I scripts/tests/test_validate_module_descriptors.py -v`
- Make targets and binaries for `unit-test-audit-worm`, `unit-test-audit-worm-chain`, and
  `unit-test-kb-audit-worm`
- `make -C src lint`
- `python3 -I -S scripts/check_cleanup_ledger.py`

The local environment does not provide `cmake` or `ctest`. A clean CMake configure/build and the
affected CTest targets therefore remain a mandatory pull-request CI gate; this record does not claim
they ran locally.

Mechanical absence checks cover all four retired flat header paths, both basename include forms,
across repository C/C++ sources, and exact old Make/CMake include-directory expressions. Review of
the implementation-source diff is limited to include-directive substitutions and formatter-required
alignment of adjacent comments; CMake variable substitution through `AIMEE_AUDIT_INCLUDE_DIR` was
reviewed against existing target visibility.

## Close-time gates

- Aimee technical-writer review was satisfied on 2026-07-22; its scope-bound consumer wording,
  mechanical-check wording, and exact local-test corrections are applied
- exact-final-diff roundtable approval
- feature-branch pull-request CI
