# Git core contract: runtime adoption residual

- **State:** DONE — archived 2026-08-04 after source ownership and the required contract/caller
  migration landed. Cross-process event adoption and rollout telemetry remain in
  [`git-core-cross-process-rollout-residual.md`](../pending/git-core-cross-process-rollout-residual.md).

**Archived parent:** [`git-core-contract.md`](git-core-contract.md)

## Archive rationale

Git now has an ownership-complete module descriptor, public contract, migrated includes/callers, and
the repository/worktree behavior tests introduced during the modularization slices. The stronger
separate-process event path and deployment compatibility gate remain independently testable work.

## Remaining deliverables

- Add runtime fixtures that exercise every required operation and failure classification.
- Migrate in-scope callers to the contract and remove incompatible behavior.
- Prove repository, worktree, detached-head, authentication, and concurrency behavior.
- Add compatibility telemetry and a rollback gate for deployment.
- Keep the contract-status checker required while the migration proceeds.
