# Hillock release-candidate qualification

The new [prerelease workflow](../../../.github/workflows/hillock-prereleases.yml) discovers all `rc/v<major>.<minor>.<patch>` branches, snapshots immutable tips and qualifies each against Aimee's HDC/subword adapter. Hourly scanning starts after the workflow reaches the default branch. Aimee PRs, pushes and releases do not trigger the scanner; manual dispatch supports one branch and forced requalification. Normal CI covers the workflow regression tests and pinned production contract.

## Local evidence

These checks ran on the PR #3020 workspace based on Aimee `eaf2dc4f4e0a62a07e242d2543546e1473413d6d`, with the workflow and runner changes present but not yet committed, using Python 3.12, pinned NumPy 2.2.6 and Go 1.26.9. The `aimee_revision` in the summaries records that base HEAD. This is local evidence; hosted Actions results are separate.

Discovery found `rc/v0.10.0` at `78364232eab6d030e90dec3ebf77b21a0616ff46`. Both this candidate and the production pin `1edd166ead75b85a9ab95cd6ba4faf7011ad567c` passed:

- Real live Go lifecycle contracts with the race detector, repeated after sidecar process restart; neither round skipped.
- Authenticated health/ranking, unsupported API rejection, malformed identity rejection and request capacity bounds.
- Cold and concurrent warm retrieval with 100, 1,000 and 10,000 synthetic records, four workers and twelve warm samples per corpus. All operations met the 30-second deadline with zero errors and zero wrong identities.

[Candidate summary](candidate-summary.json), [candidate scale results](candidate-scale.json), [pinned summary](pinned-summary.json) and [pinned scale results](pinned-scale.json) preserve revisions and measured results. Candidate cold retrieval took 117/338/605 ms; maximum concurrent warm latency was 466/1,279/3,217 ms for the three corpus sizes. These synthetic lexical fixtures do not establish general conversational quality.

Seven discovery/runner regressions passed, including general branch matching, numeric version order, changed tips, malformed/duplicate tips, missing manual branches, matrix overflow, network failure, immutable revisions, source modifications, production-pin isolation and failed-run evidence. Memory/core, owner, catalog, Cognee/Hillock adapter and egress package race tests passed. The workflow passed actionlint and documentation links passed `scripts/check-docs.py`.

## Operation and limits

Only an exact successful receipt for upstream branch and Hillock SHA skips repeat qualification. New upstream branches/tips require another run; Aimee commits do not invalidate receipts. Candidates without successful receipts retry next scan and cache eviction reruns safely. Manual force explicitly retests an unchanged candidate against current Aimee code. Receipts retain the Aimee SHA actually tested. Success receipts contain no credentials or source/corpus data. Fixture logs and results are retained for 30 days, and failures remain visible as failed workflow jobs. Deleted branches disappear from the next discovery snapshot.

Candidate qualification changes only expected revision constants in temporary sidecar copies and a Go build overlay. Production source and Docker pins are unchanged. A passing candidate needs a reviewed pin update before deployment. Upstream chat/graph/learning and the remaining real gateway/model qualification work on PR #3020 are outside this gate.
