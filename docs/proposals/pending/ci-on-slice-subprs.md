# Proposal: run CI on slice sub-PRs

- **State:** pending — single slice.

## Problem

The build-e2e workflow merges each slice through a sub-PR whose stated gate is
"sub-PR -> GREEN CI -> merge into the feature branch". In practice no CI runs.

`.github/workflows/ci.yml` triggers on:

```yaml
pull_request:
  branches: [main, testing, feature/core-modularization]
```

Slice sub-PRs target `aimee/feat/<work_item_id>`, which is not in that list.
Observed on PR #2011, opened and merged by the pipeline: `check-runs` total_count
= 0. The engine correctly advanced — `wfe_live_forge.c` deliberately treats
GIT_PR_CI_NONE as merge-permitting so an intermediate PR cannot park forever —
so the gate passed because there was nothing to check.

Consequence: every slice merges into the feature branch unverified. A slice can
break the build and only the final feature->testing PR discovers it, after all
slices have already merged, with no attribution to the slice that caused it.

## Options

1. Add `aimee/feat/**` to the `ci.yml` pull_request branch list. Slices get the
   full existing gate. Cost: one full CI run per slice; a 5-packet run becomes 5
   additional CI runs.
2. Add a reduced job (build + unit tests only) for `aimee/feat/**`, deferring
   e2e/docker legs to the final PR. Cheaper; catches compile and unit breakage at
   the slice that caused it.
3. Leave as-is and rely on the final PR. Cheapest; loses per-slice attribution
   and lets a broken slice merge.

## Recommendation

Option 2. The value of slice-level CI is attribution — knowing which slice broke
the build — and build+unit tests deliver most of that at a fraction of the cost.
The expensive e2e legs still run once on the final PR, where they gate delivery.

## Acceptance

- A slice sub-PR targeting `aimee/feat/**` reports at least one check run.
- A slice that breaks the build fails its own sub-PR rather than the final one.
- The final feature->testing PR still runs the complete gate unchanged.
