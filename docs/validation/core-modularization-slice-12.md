# Core modularization slice 12: baseline and cleanup foundation

## Outcome

This slice freezes the current refactor-sensitive surfaces before broader source movement and adds
one strict cleanup ledger for completed implementation slices. Both contracts are deterministic,
repository-local, and CWD-independent.

`tests/baselines/refactor/index.json` records sorted membership and normalized SHA-256 digests for
CLI help, routes, configuration, public headers and declared symbols, plugin ABI headers, database
schema/migration inputs, and package/install inputs. The checker reports the first canonical drift
and points maintainers to the explicit freeze command. CI only checks; it cannot silently refresh
the baseline.

`tests/baselines/refactor/cleanup-ledger.json` records factual entries for the background-curator
deletion (Slice 5), the landed physical ownership and plugin-loader work (Slices 8–11), and this
foundation. Slices without committed evidence are deliberately not backfilled. Unknown fields,
duplicate or unordered slices, missing evidence, and unverified entries fail closed.

## Scope boundary

This is source-level mechanical protection. It does not connect to a database or network service,
execute migrations, depend on compiled binaries, or encode timestamps, Git revisions, paths, or
toolchain fingerprints. The plugin ABI projection freezes authoritative headers; compiled
size/alignment compatibility remains a later integration concern. This slice adds no production
source and implements no alias, recovery runner, signed descriptor-v2, Git migration, or source
relocation.

## Verification

- `python3 -I -S scripts/refactor_baselines.py`
- `python3 -I -S scripts/check_cleanup_ledger.py`
- `python3 -I scripts/tests/test_refactor_baselines.py -v`
- `python3 -I scripts/tests/test_check_cleanup_ledger.py -v`
- `make -C src lint`
- feature-branch pull-request CI

The roundtable approved the baseline-and-cleanup foundation as the next prerequisite slice. The
exact implementation returns to technical-writing and roundtable review after local verification.
