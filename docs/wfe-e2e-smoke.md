# WFE E2E Smoke: build-e2e

This note documents the end-to-end smoke path for a single `build-e2e` workflow run.

## Launch

Launch the workflow with the exact command:
`aimee workflow run build-e2e --proposal <file>`

## Monitor progress

Check progress with the exact command:
`aimee workflow status <id>`

Add `--watch` to follow the run in real time.

## Stage flow

The workflow advances through these stages in order:

1. **draft** – author the initial proposal.
2. **feature branch** – create a branch for the work.
3. **plan** – turn the proposal into an implementation plan.
4. **plan roundtable** – review the plan with a roundtable panel.
5. **split** – divide the plan into slices.
6. **per-slice child workflows** – run a child workflow for each slice.
7. **acceptance roundtable** – review the combined result.
8. **documentation gate** – write and verify documentation.
9. **final PR** – open a pull request with the completed change.

## Final state

The workflow ends with an **opened pull request**. The PR is intentionally **not merged**; the operator merges it manually.
