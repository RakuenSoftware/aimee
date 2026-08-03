# Workflow autonomy runbook

A parked run is preserved evidence, not a failed retry. Use this runbook to repair the named
constraint and continue the same work item.

## Triage

1. **Open the work item.** Use **Workflow Actions**.
2. **Record its boundary.** Capture state, node, park reason, version, artifact hash, repository, and spend.
3. **Find the first failure.** Read the latest events and the first failed external attempt.
4. **Check that node's dependencies.** Inspect agent admission, provider, worktree, verification, and forge health.
5. **Repair and resume.** Continue the same work item.

The default build opens a draft final PR against the branch checked out when the repository was
admitted. That integration lane can differ from the forge default. The workflow does not mark the PR
ready or merge it automatically. The draft must have a proposal-derived title and include the
original request, approved plan, diff summary, slice PRs, completed gates, and human-review boundary.

If a final PR is non-draft, has a work-item ID for a title, lacks that review context, or was merged
without the explicit human handoff, treat the run as failed even when its lifecycle row says
`accepted`. Preserve the PR and lifecycle audit trail, revoke any agent-accessible write credential,
and verify repository protection before running another live workflow.

## Never force past

- **Human decision:** a human gate.
- **Artifact identity:** a missing or changed hash.
- **Implementation evidence:** a failed or missing commit.
- **Repository state:** a merge conflict or forge confinement failure.
- **Verification:** non-green required CI.
- **Review authority:** a degraded panel without quorum.
- **Final handoff:** the draft state of a final workflow PR.

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

The pending-proposal watcher scans the refreshed git ref on every poll. Five rules decide admission:

1. **Hash proposal bytes, workflow, and mode.** The watched commit is not part of identity.
2. **Ignore branch-only movement.** A new commit with unchanged proposal bytes does not duplicate a run.
3. **Admit changed proposals again.** New bytes create a new identity for the next scan.
4. **Queue at the cap.** An eligible proposal waits for a later scan instead of being rejected.
5. **Retry queued work.** The watcher reconsiders it without a manual fire.

Edit the cap under **Workflows > Run policy**. A value of `0` pauses new watched-proposal and
browser-submit admission.
