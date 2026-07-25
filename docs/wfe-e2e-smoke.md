# WFE E2E Smoke: build-e2e

This note describes the end-to-end smoke path for the `build-e2e` workflow.

## Launch

```bash
aimee workflow run build-e2e --proposal <file>
```

The command returns a work-item ID.

## Monitor progress

```bash
aimee workflow status <id>
```

Add `--watch` to follow the run in real time.

## Stage flow

The workflow advances through the following stages in order:

1. **draft** – the initial proposal is authored.
2. **feature branch** – a branch is created for the work.
3. **plan** – the proposal is turned into an implementation plan.
4. **plan roundtable** – the plan is reviewed by a roundtable panel.
5. **split** – the plan is divided into slices.
6. **per-slice child workflows** – each slice runs its own child workflow.
7. **acceptance roundtable** – the combined result is reviewed.
8. **documentation gate** – documentation is written and verified.
9. **final PR** – a pull request is opened with the completed change.

## Final state

The workflow ends with an **opened pull request**. The PR is intentionally **not merged**; the operator must merge it manually.
