# 0.4.5 release preparation

The version resolver selects **0.4.5** after **v0.4.4**. Publication has not been
approved or performed by this preparation. The promotion PR is
[#2991](https://github.com/RakuenSoftware/aimee/pull/2991).

## Verified implementation

The build and inventory corrections in
[#2992](https://github.com/RakuenSoftware/aimee/pull/2992) passed all 59 checks at
`620de76472798ff335aa0281dc2792df4dea07b9`. The
[CI run](https://github.com/RakuenSoftware/aimee/actions/runs/35718797295)
passed native and Go unit tests, all three PostgreSQL shards, sanitizers, the
complete script suite, Linux and macOS real-provider LSP checks, platform builds,
and all four Docker deployment tiers. Pins, release policy, fuzzing and benchmark
smoke also passed. The merge into `testing` is
`4cbfd3ef95f285d96bf066f008832f2b8ba2f1e4`.

Native graph enum consumers use the public declaration header with an include
path scoped to those consumers and their alternate test builds. The LSP build
contract and validator retain the frozen compiler commands. The source boundary,
declaration ledger and link-closure transitions account for the Go memory cutover.

The full local unit run had failures in the unchanged SQLite allocation-failure
classification and Git credential environment tests. Local GCC 16 required C17
and warning compatibility settings. The passing CI results above provide the
complete supported-runner validation; the local run is not recorded as passing.

## Promotion documentation repair

The main-only module inventory job reached documentation checks after the build
corrections and rejected migration history as an orphan module document. The
memory guide also had extra top-level sections and omitted three dependencies
declared by its descriptor. The benchmarks guide had an extra top-level section.

The canonical module guides now retain the required section order and complete
dependency declarations. Detailed memory behavior remains in
[the behavior guide](../MEMORY.md), and the implementation chronology remains in
[migration history](memory-migration-history.md). All inbound links are updated.
The documentation checker and its required sections remain unchanged.

All 40 applicable `module-inventory` job steps passed locally against
`origin/main`. The link-closure suite's 75 tests were rerun outside the sandbox
because its sanitizer fixture cannot run under ptrace. All 35 module documents,
the 13 documentation failure-mode tests, test registration, and the Git core
contract pass. General documentation validation passes in an isolated checkout
that excludes unrelated untracked drafts.

Main's existing merge commit is incorporated without changing the implementation
tree, so the promotion can satisfy the up-to-date ancestry requirement.

## Remaining release steps and limits

The documentation correction still requires integration into `testing` and a
fresh successful main-promotion run. Versioned client and image validation must
finish before the protected `main-merge-approval` environment can be approved.
Publication has a separate protected `release` environment. Required independent
boundary review and reviewer coverage remain governed by `OWNERS.md`.

Complete DB2 retirement is separate from the Go memory cutover. Native DB2
consumers remain; this release preparation does not certify their removal.
The scheduled native performance workflow still invokes five retired C memory
benchmarks and exits without certifying a baseline. That workflow is outside the
PR gate. Passing Go retrieval evaluation does not establish equivalence to those
historical native performance measurements.
