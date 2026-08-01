# Workflow autonomy runbook

Use this when an autonomous workflow stops or behaves unexpectedly.

## Triage

1. Open the work item in **Workflow Actions**.
2. Record state, current node, park reason, workflow version, artifact hash, repository, and spend.
3. Read the last lifecycle events and the first failed external attempt.
4. Check agent admission, provider, worktree, verification, and forge health for that node.
5. Repair the named condition and resume the same work item.

The default build opens a final PR; it does not merge the repository default branch automatically.

## Never force past

- a human gate;
- a missing or changed artifact hash;
- a failed or missing implementation commit;
- a merge conflict;
- non-green required CI;
- a degraded panel without quorum;
- a forge identity or branch-confinement failure.

## Recovery

The Go control plane persists transitions before dispatch. Restart is safe when the process is
wedged: supervision stops both server peers, then startup reconciles reservations and active items.
Inspect the item after restart before retrying manually.

Repeated feedback or exhausted rounds parks by design. Narrow the request, fix the workflow, or make
a human decision; raising every limit usually spends more without creating progress.

## Evidence

Keep the work-item ID, request IDs, lifecycle events, artifacts, provider attempt, verification log,
and audit result. Tool and credential paths are audited through the event bus; trigger delivery is
not yet an integrated bus consumer.
