# Git core contract: runtime adoption residual

- **State:** PENDING — residual scope only.

**Archived parent:** [`git-core-contract.md`](../done/git-core-contract.md)

## Remaining deliverables

- Add runtime fixtures that exercise every required operation and failure classification.
- Migrate in-scope callers to the contract and remove incompatible behavior.
- Prove repository, worktree, detached-head, authentication, and concurrency behavior.
- Add compatibility telemetry and a rollback gate for deployment.
- Keep the contract-status checker required while the migration proceeds.
