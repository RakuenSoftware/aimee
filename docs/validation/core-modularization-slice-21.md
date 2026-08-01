# Core modularization slice 21: module-runtime ownership pilot

## Scope

This slice starts from `46e78ffeb4631b5fd052a183e101e79d4fa9dfd8` and pilots four optional,
per-descriptor ownership fields on required `module-runtime`: `sources`, `public_headers`, `tests`,
and `docs`. Each field is independently optional; a descriptor may declare any subset, including
none, without failing validation. This slice does not move files, generate build inputs, alter
module selection, enrich another descriptor, or change runtime behavior.

## Contract

Each declared path is a normalized repository-relative path to an existing regular file. Sources
must remain below the declaring module, public headers below its canonical installed-header tree,
tests below `src/tests` with the `test_` convention, and docs at the module's single canonical
document. Absolute paths, traversal, declared paths whose real path differs from their lexical path
(symlinks rejected before claim), wrong roles or extensions, within-role duplicates, cross-role
claims, and reuse of the same normalized lexical path by different descriptors fail closed with a
stable rule and JSON pointer.

Per-descriptor optionality is a deliberate migration concession. The `--emit-ownership` projection
emits a byte-stable JSON report that lists every descriptor and every ownership role, sorts
descriptors by module ID, and preserves declared role-list order within each module. An empty role
appears in the report and means only that the descriptor has not yet been enriched; it is not a
claim of complete ownership.

## DRY and build boundary

One shared validator owns path normalization, containment, role, existence, duplicate, and report
rules. The pilot inventory names the already-canonical `module-runtime` sources and headers and the
focused C-hook test; it does not claim the mixed plugin-loader test. Make and CMake remain
authoritative build-input lists until a later slice can replace both with one generated projection.
Until then, the four ownership fields are documentation and validation only: the build does not read
them, and they do not change what is compiled or installed.

## Completed local verification

- `python3 -I -S scripts/validate_module_descriptors.py --check-schema src/modules`
- `python3 -I -S scripts/validate_module_descriptors.py --check-schema --emit-ownership src/modules`
- `python3 -I scripts/tests/test_validate_module_descriptors.py -v`
- `python3 -I -S scripts/check_capability_ownership.py`
- `python3 -I -S scripts/check_cleanup_ledger.py`
- `python3 -I -S scripts/refactor_baselines.py`
- `make -C src lint`

## Close-time gates

- technical-writer review and exact-final-diff roundtable approval
- feature-branch pull-request CI
