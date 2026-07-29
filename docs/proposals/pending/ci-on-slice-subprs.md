# Proposal: run CI on slice sub-PRs

- **State:** implemented — option 1 shipped in `ci.yml`.

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
`GIT_PR_CI_NONE` as merge-permitting so an intermediate PR cannot park forever —
so the gate passed because there was nothing to check.

Consequence: a slice sub-PR whose target branch is not in the trigger list merges
into the feature branch with no check runs. A slice can break the build and the
final feature->testing PR is the first gate that would discover it, with no
attribution to the slice that caused it.

## Established

By direct observation:

- Before this change, `ci.yml`'s `pull_request.branches` list did not include
  `aimee/feat/**`.
- PR #2011 merged with `check-runs` total_count = 0.
- `GIT_PR_CI_NONE` is defined by the `git_pr_ci_t` enum in
  `src/modules/git/git_pr_api.h`, and the `GIT_PR_CI_NONE` case in
  `src/modules/workflows/wfe_live_forge.c` records why the engine treats no
  reported CI as merge-permitting. This is verified design intent, cited to the
  defining enum and handling case rather than inferred from behaviour.
- **`RakuenSoftware/aimee` is a public repository** (`gh repo view`:
  `"visibility":"PUBLIC"`). GitHub-hosted *standard* runners are free for public
  repositories; the per-OS billing multipliers (Linux 1x, Windows 2x, macOS 10x)
  apply to private-repo minute consumption, not here.
- **Job structure at measurement time:** the measured runs below had **15 job
  definitions** and **17 executions** after the `e2e-docker` T1/T2/T3 matrix
  expansion. These figures are a historical snapshot, not a verification
  invariant: the workflow has gained jobs since the measurements were taken.
  Verification of this trigger-only change must cover every job and every matrix
  expansion present in `ci.yml` at verification time.

### Measured job durations

Elapsed wall-clock per job, from the Actions jobs API for the two most recent
complete `ci.yml` runs. These are **elapsed minutes, not billed minutes** — see
above; nothing here is billed.

| Job | Runner | Run 30221629787 | Run 30221474744 |
|---|---|---|---|
| no-coauthor-trailers | ubuntu | 0.16 | 0.16 |
| lint | ubuntu | 0.96 | 1.03 |
| build | ubuntu | 1.96 | 2.06 |
| build-integrity | ubuntu | 4.90 | 4.90 |
| unit-tests | ubuntu | 5.86 | 5.83 |
| init-migrate-service-test | ubuntu | 1.16 | 1.20 |
| windows-cmake | windows | 4.20 | 4.06 |
| windows-build | windows | 3.36 | 3.51 |
| macos-build | macos | 0.61 | 0.53 |
| memory-retrieval-eval | ubuntu | 0.76 | 0.56 |
| bench-check | ubuntu | 0.68 | 0.63 |
| treesitter | ubuntu | 0.96 | 1.13 |
| vault-pkcs11-token | ubuntu | 0.48 | 0.38 |
| e2e-adoption | ubuntu | 1.80 | 1.83 |
| e2e-docker (T1) | ubuntu | 8.05 | 8.00 |
| e2e-docker (T2) | ubuntu | 9.73 | 10.01 |
| e2e-docker (T3) | ubuntu | 3.00 | 3.18 |
| **Sum of all jobs** | | **48.63** | **49.00** |
| `build` + `unit-tests` only | | **7.82** | **7.89** |
| Longest single job (= wall-clock floor) | | 9.73 (e2e-docker T2) | 10.01 (e2e-docker T2) |

Derivation: the "sum" rows add the column above them; the option-2 row adds only
`build` and `unit-tests`. Jobs run concurrently, so the sum is total machine
occupancy, not elapsed time — elapsed time is bounded below by the longest job.

**n = 2.** These are point estimates from two runs. They do not characterise
variance, which for hosted runners is driven largely by queue and runner
availability. Treat the figures as indicative, not as a cost model.

## What the measurements actually show

Since minutes are free for this repo, the sum-of-jobs figure is **not a cost**.
The real costs of adding slice CI are:

1. **Wall-clock added per slice.** The full gate's *total workflow elapsed time*
   is measured, from three complete runs:

   | run | elapsed |
   |---|---|
   | 30223112295 (`e161dd34`, dispatched) | 9m45s |
   | 30221629787 | 10m07s |
   | 30221474744 | 10m21s |

   So a full slice gate costs **~10 minutes** of wall-clock (n=3).

   The corresponding figure for option 2 is **not measured** — that
   configuration does not exist, so no run of it can be timed. Its elapsed time
   is bounded *below* by its longest job (`unit-tests`, ~5.85 min) plus queue and
   setup overhead, which the job table does not capture. Consequently the saving
   from option 2 is **at most ~4 minutes per slice, and probably less**. Treat
   that as an indicative upper bound on the benefit, not a measured difference.
2. **Runner concurrency.** At measurement time, a 5-slice run added 5 x 17 =
   **85 job executions** competing for the account's concurrent-job allowance
   against all other CI in flight. This historical estimate scales with the
   complete workflow and must be recalculated from the jobs and matrix expansions
   present in `ci.yml` when capacity is evaluated. Runner concurrency is the one
   genuine scarcity, and it is **not quantified here**: the account's concurrency
   ceiling and its current utilisation were not measured. If slice CI is adopted
   and other PRs start queueing behind it, this is the cause to look at first.

## Options

1. Add `aimee/feat/**` to the `ci.yml` `pull_request.branches` list. Slices get
   the full existing gate.
2. Add a reduced job (build + unit tests only) for `aimee/feat/**`, deferring
   e2e/docker legs to the final PR.
3. Leave as-is and rely on the final PR.

## Recommendation

**Option 1.**

The case for option 2 was cost, and for this repository that cost does not
exist: minutes are free. What option 2 actually buys is at most ~4 minutes of
latency
per slice and a reduction in concurrent job slots. What it costs is real:
partial attribution (e2e-only and cross-slice regressions still surface only at
the final PR) and a second CI configuration that must be kept in sync with the
first — a standing source of drift where the slice gate and the delivery gate
silently diverge.

Trading complete attribution and a single source of truth for four minutes is a
bad trade when nothing is being billed. Option 1 also needs no new job
definitions: it is one line added to an existing trigger list.

If runner concurrency later proves to be the binding constraint — the one cost
above that is real but unmeasured — option 2 remains available as a targeted
remedy, and should be revisited with concurrency data in hand rather than
adopted pre-emptively now.

**Post-adoption measurement, with an explicit trigger for reconsidering.**
Because concurrency is knowingly unquantified at decision time, adopting option 1
carries an obligation to measure it afterwards rather than leave it open.

The requested pre-enablement `Q0` sample was not captured before commit
`968c804d` enabled slice CI. Do not invent that missing baseline or compare a
post-adoption sample to an unspecified value. Replace it with a retrospective,
mechanically comparable baseline from GitHub Actions: select the 10 completed
`pull_request` runs of `ci.yml` whose base branch is not `aimee/feat/**` and whose
`created_at` values are the latest before the `968c804d` commit time. For each
run, calculate queue wait as the earliest job `started_at` minus the run
`created_at`; `Q0` is the arithmetic median (the mean of the fifth and sixth
ordered values) of those 10 waits. Record the commit time, run IDs, timestamps,
per-run queue waits, API retrieval date, and resulting median in this proposal
before evaluating the threshold below.

For `Q1`, use the first 10 completed slice gates created after slice CI was
enabled. Measure the queue wait of every unrelated `pull_request` `ci.yml` run
(base branch not `aimee/feat/**`) created from the first slice gate's `created_at`
through the last slice gate's `updated_at`, using the same earliest-job-start
calculation and arithmetic-median rule; `Q1` is the median of those values. Also
record maximum concurrent job executions reached (`Cmax`) and total elapsed time
of each slice gate. Preserve the run IDs and timestamps here so the sample can be
reproduced from the Actions API. If the window contains fewer than 10 unrelated
PR runs, extend its end to the creation time of the tenth subsequent unrelated
PR run and use those 10 runs for `Q1`.

**Reconsider option 2 if any of these is true** (each mechanically checkable):

- `Q1 >= Q0 + 2 minutes`; **or**
- slice gate elapsed time exceeds **20 minutes** in **3 or more** of the 10 runs
  (against the measured ~10-minute standalone figure); **or**
- `Cmax` reaches the account's documented concurrent-job ceiling in **2 or more**
  of the 10 runs. (The ceiling must be read from the account's plan limits at
  measurement time; it is not known here, which is precisely why it is recorded
  rather than assumed.)

If none of the three fires across those 10 runs, close the question and record
concurrency as measured and adequate.

## Acceptance

Common to any option that adds slice CI:

- A slice sub-PR targeting `aimee/feat/**` reports at least one check run.
- The final feature->testing PR still runs the complete gate unchanged.

If **option 1** is implemented:

- A slice that breaks *any* gated leg — including an e2e or docker leg — fails
  its own sub-PR rather than the final one.
- No new workflow file or job definition is introduced; the change is confined
  to `ci.yml`'s trigger list.

If **option 2** is implemented instead:

- A slice that breaks the build or a unit test fails its own sub-PR.
- An e2e-only or cross-slice regression is still expected to surface only at the
  final PR. This is an accepted limitation of option 2 specifically, and must be
  recorded as such so the weaker guarantee is not mistaken for the full gate.

## Implemented change

`ci.yml` now includes `'aimee/feat/**'` in the existing `pull_request` branch
filter. A pull request targeting a work-item feature branch therefore starts the
same workflow as pull requests targeting `main`, `testing`, or
`feature/core-modularization`. No job, matrix, permissions, event type, or
workflow-engine CI semantics changed.

## Verification

1. Parse `.github/workflows/ci.yml` and confirm the `pull_request.branches`
   filter includes `aimee/feat/**` while retaining all previous target branches
   and pull-request activity types.
2. Compare the workflow before and after the implementation and confirm that
   every job definition and every matrix expansion present in `ci.yml` at
   implementation time remains unchanged; do not use historical hard-coded job
   counts as the gate.
3. Open or synchronize a test pull request targeting an `aimee/feat/<work-item>`
   branch and confirm that all executions produced by the complete current
   workflow report check runs. Confirm a feature-to-`testing` pull request still
   produces that same complete gate.
