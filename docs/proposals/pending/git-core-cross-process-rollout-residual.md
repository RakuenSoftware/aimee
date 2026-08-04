# Git core contract: cross-process rollout residual

- **State:** PENDING — residual scope created by the 2026-08-04 proposal audit.

**Archived source:** [`git-core-contract-runtime-residual.md`](../done/git-core-contract-runtime-residual.md)

## Delivered baseline

Git has an ownership-complete descriptor, public contract, migrated source ownership/callers, and
fixtures for core repository and worktree behavior.

## Remaining deliverables

- Serve Git operations through the module event contract from a separate program.
- Prove repository, worktree, detached-head, authentication, cancellation, and concurrency failures.
- Preserve principal, repository, producer, and redaction provenance on memory ingest records.
- Add compatibility telemetry, staged enablement, rollback thresholds, and deployment proof.
- Reject direct callers that bypass the event contract.

## Completion evidence

Runtime fixtures must exercise every operation and failure class through the bus and show equivalent
accepted behavior during migration, including a tested rollback after partial enablement.
