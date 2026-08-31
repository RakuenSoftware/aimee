# Compaction quality: committed baseline

- **State:** PENDING. Residual scope only.

**Archived parent:** [`compaction-quality-measurement.md`](../done/compaction-quality-measurement.md)

## Remaining deliverables

- Run the existing agentic harness with today's compactor over a documented, reproducible corpus.
- Commit per-session and aggregate rounds-to-resume, read-only re-derivation, retained-byte, and boundary counts.
- Record the exact commit, configuration, harness version, dataset identity, and exclusions.
- Establish comparison and regression thresholds for future context engines without changing compaction behavior in this slice.

## Acceptance

The baseline artifact is reproducible from a checked-in command, contains enough samples to expose variance, and is consumed by a deterministic comparison check.
