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

## Automatic proposal admission

The autonomous pending-proposal watcher scans the watched branch on every poll and decides whether
each pending proposal is eligible for a new run. The decisions are governed by five behaviors:

1. **Identity derivation.** Pending-proposal identity is derived from the complete proposal file
   bytes together with workflow and mode. The watched-branch commit is not part of the identity
   and is therefore not sufficient on its own to identify a pending proposal.
2. **No duplicate runs on branch-only advances.** Advancing the watched branch without changing the
   proposal bytes does not start a duplicate run. The watcher treats the unchanged bytes as the
   same pending proposal and leaves its prior run status intact.
3. **Byte changes re-eligibility.** Changing the proposal bytes produces a new pending-proposal
   identity, which makes the proposal eligible for a new run on the next scan.
4. **Admission cap queues eligible proposals.** The live trigger admission cap can queue an
   otherwise eligible proposal when the cap is reached on a given scan. The cap is edited in
   **Edit Workflows → Run policy**; an otherwise eligible proposal that exceeds the cap is held
   for a later scan instead of being admitted or rejected.
5. **Cap-queued proposals stay eligible on later scans.** A proposal queued by the cap remains
   eligible on a later scan; the watcher does not require manual firing to retry a proposal that
   was only held back by the cap. As long as the proposal bytes, workflow, and mode are
   unchanged, it is reconsidered on every subsequent scan until the cap admits it.
