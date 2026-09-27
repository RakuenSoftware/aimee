# 0.4.6 release preparation

The next version in the declared 0.4 series is **0.4.6**, following released v0.4.5.
The promotion is [PR #2997](https://github.com/RakuenSoftware/aimee/pull/2997).
Publication and production rollout are pending the versioned release gates.

## Candidate and verification

[PR #2990](https://github.com/RakuenSoftware/aimee/pull/2990) merged into testing as
`7956440a619ad607fa7d69133af9ffc961ef7bea`. Its executable tip
`38e3ada87e6df933e13e032415fd49b81e36be7e` passed all **55 CI jobs** in
[run 36312040587](https://github.com/RakuenSoftware/aimee/actions/runs/36312040587).
Benchmark smoke, fuzz, repository pins, and release policy also passed. This includes native
and PostgreSQL tests, sanitizers, frozen retrieval evaluation, four deployment topologies,
and published-version upgrade/rollback checks.

The exact executable commit passed the full T2 deployment on isolated CT109 on .253:
215 topology assertions, 107 native async assertions, 352 provider-boundary assertions,
321 shared-memory assertions, and six identity assertions, with zero failures. Parent-first
fixture cleanup also passed ten repeated typed-source rounds. Results remain on CT109 under
`/opt/pr2990-evidence/ci-t2-38e3ada87`; private stores and credentials are not published.

The race-enabled memory owner and evaluator packages passed locally. The module package passed
with a short build-scratch path after a local Unix socket path-length failure. An earlier
concurrent local owner run timed out in graph retrieval; the isolated regression, sequential
full memory-owner run, and supported CI replay passed. The frozen release gate executed all
173 required Go and 40 required Python tests without skipped required cases.

## Promotion changes

Main's released ancestry is incorporated without changing the merged implementation tree.
The public-surface baseline is regenerated once for this promotion. Its diff records the new
memory schemas, source-version contracts, configuration, and documented public surface.

The DB2 comparison accounts for the exact header-only integer codec already used by the
retained retrieval-reference and fidelity-attribution writers. This preserves 64-bit source identities instead of
rounding them through JSON doubles. The admission requires the same retained source on both
sides, exact header resolution, host-API classification, and one include. Negative tests
reject another source, another header, retargeting, class changes, count growth, and retirement.
The bounded trace reader also accounts for its exact libc `strlen` reference, needed to reject
truncated stored payloads. Neither admission adds a host operation or broadens DB2 runtime privileges.

The final preparation passes the source-lock check, public-surface baseline check, documentation
check, and comparisons against main. All 25 boundary and 76 linkage tests pass. The real
linkage probe passes on Ubuntu 24.04 with PostgreSQL 18 headers, matching the CI ABI;
local newer-glibc and Debian probes differed and were not used to rewrite the contract.

## Release limits and rollout

MR-07 enforcement promotion remains explicitly deferred. MR-09 selection and MR-10 utility
horizons remain disabled; MR-17 clean retry remains opt-in. Deterministic acceptance does not
claim model-quality, latency, or estimated-cost noninferiority. Earlier proposal evidence stays
tied to its original revision, including the pre-CI MR-18 closeout image.

Production CT100 was confirmed healthy on released 0.4.5 before preparation. The rollout must
validate the published 0.4.6 artifacts against retained 0.4.5 stores on CT109, preserve Vault
and client identities, retain recoverable database snapshots, and verify the local thinclient
after CT100 deployment. Do not relabel this preparation as a completed production upgrade.
