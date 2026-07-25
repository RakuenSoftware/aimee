# WFE Build-E2E Smoke Runbook

## Purpose / When to use

This is an operator note for driving a single end-to-end build-e2e workflow run, from proposal through an opened PR (the run deliberately does not merge it).

## Stage flow

1. draft
2. feature branch
3. plan
4. plan roundtable
5. split into slices
6. per-slice child workflows
7. acceptance roundtable
8. documentation gate
9. final PR (opened, not merged)

## How to launch

Trigger the parent build-e2e workflow from the workflow run list. Provide the proposal reference, target repo/branch, and any required labels or parameters. Launching produces one parent workflow plus N child workflows — one per slice.

## How to check progress

Watch the parent run in the workflow run list or at its run detail URL. The child runs are linked from the parent, one per slice. At each stage check status, current stage, and last completion timestamp. Expect intermediate artifacts: plan output, slice manifests, acceptance verdict, and docs-gate result.

## What a healthy run looks like

All stages advance in order without manual intervention. Each child workflow completes successfully for its slice. Acceptance roundtable passes, the documentation gate passes, and a final PR is opened against the base branch in opened state — not merged, not closed.

## Where it parks for a human

The run stops after the final PR is opened, awaiting human review and merge. The opened PR is the human handoff point; the operator does not auto-merge. Look at the opened PR URL and the workflow run summary.

## Failure / stall signals

- A stage stops advancing within the expected window; the run parks at that stage for inspection.
- A child workflow fails or the acceptance roundtable rejects; the run surfaces the failing step for human inspection.
