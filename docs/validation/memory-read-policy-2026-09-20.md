# MR-01 direct reads and explicit temporal modes

PRs: [#2988](https://github.com/RakuenSoftware/aimee/pull/2988) and
[#2989](https://github.com/RakuenSoftware/aimee/pull/2989).

Current exact-ID reads now enforce the same normalized validity, suppression and
lifecycle gates as search. Legacy `as_of` inspection retains old versions while
withholding erased, rejected, revoked, quarantined and hidden content.

The separate version-one `read_policy` contract adds strict current/historical
selection. Historical KB reads return a retained version only inside its
half-open valid interval. Unsupported belief time, personal history, modes,
versions and operations return explicit errors. The new temporal contract adds
no database round trip; its interval predicate is part of the record query.
No latency improvement is claimed for this correctness change.

## Fresh shipping-image validation

Built `Dockerfile.server` with `WITH_VSCODE=0` from implementation commit
`d05d798e0439cd4e4aa6a9a022f991ef88a5349e` in the owned CT 9498 on `.253`.
Image: `sha256:c953501847ccab2fa24dc186b5c2fecdfaf6363cc34d90b8be7f21d734fa43ab`.
The subsequent `177318b5a9` change registers the already-built Go source in the
module descriptor and formats a native transport test; it changes no serving
logic. CI validates that follow-up separately.

`tests/e2e/deployment-matrix.py --topology T2` created fresh application identities,
storage and Compose projects using the retained PostgreSQL image
`aimee-postgres:pr2983` and pinned real Bekko-a25m embedder
`sha256:b03199bee881bf632f7194f472de7bc370e66d16b7215bb6aa506fb2b1510209`.

All **224 verdicts passed**: 17 topology, 51 standalone personal, 150 shared
placement and 6 immutable-identity checks. The HTTP tests cover both historical
endpoints, excluded intervals, preserved Unicode content, current private reads,
and HTTP 400 for unsupported schemas/modes. Existing CLI/MCP, scope isolation,
restart, disconnected-module and dependency-outage regressions also passed.
The runner removed its owned test containers/networks after completion.

[Content-free verdict receipt](memory-read-policy-2026-09-20/fresh-t2.json).

## Local and CI evidence

- Full memory and runtime-web race suites passed against PostgreSQL fixtures.
- Restricted non-owner replay passed at 384 and 1024 dimensions, including
  normalized offsets, exact boundaries, hidden IDs, excluded lifecycles and
  malformed governed times. The original expired direct-read regression failed
  against the merged baseline before the first fix.
- Native server forwarding regression, `CGO_ENABLED=0` module build, source
  registration, ownership and pure-Go checks passed.
- PR #2988 head `6fd0b86e3fb177fe3b9d940cb85a787498a48ef0` passed all 57 checks.
  PR #2989 CI remains separate from these tested implementation receipts.

These are MR-01 slices, not certification of all MR-01 or MR-18 requirements.
Shared evidence decisions, host capability vocabulary, validity CLI and final
release-generation checks remain in the delivery ledger. Memory stays Go; the
C bus is unchanged.
