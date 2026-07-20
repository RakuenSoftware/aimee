# Sandbox E2E Proof

This document proves that the autonomous WFE (Workflow Engine) sandbox
pipeline works end to end: a work item is planned, split into units,
delegated to a sandboxed executor, verified on the worktree, and committed
to its branch with no manual intervention. The presence of this file on
the work-item branch is the artifact that closes the loop; the embedded
run marker below ties it back to the overnight run that produced it.

## Run Marker

`overnight-e2e-run23-converge-58029`

## What this proves

- The worktree for the work item was provisioned and the branch was
  created from the agreed base.
- The approved plan was executed against the worktree by a delegated
  agent running inside the sandbox.
- Verification (`aimee git verify`) accepted the change against the
  acceptance criteria for this work item.
- The change was committed on the work-item branch and is visible in
  the repository's history.

This file is intentionally self-contained: it requires no external
references, no fetched resources, and no sibling documents to be
understood. It stands as a single, auditable proof point for the
overnight run whose marker it carries.
