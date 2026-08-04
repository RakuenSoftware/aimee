# Proposal: run CI on slice sub-PRs

- **State:** DONE — archived after the implementation and rollout decision were completed.
- **Historical state:** implementation prepared — option 1 is present in `ci.yml`; rollout remained
  blocked until the pre-enablement Q0 baseline was recorded.

## Problem

Before option 1 was implemented, the build-e2e workflow merged each slice through
a sub-PR whose stated gate was "sub-PR -> GREEN CI -> merge into the feature
branch", but in practice no CI ran.

At that time, `.github/workflows/ci.yml` triggered on:

```yaml
pull_request:
  branches: [main, testing, feature/core-modularization]
```

Slice sub-PRs targeted `aimee/feat/<work_item_id>`, which was not in that list.
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

**Rollout measurement and activation sequence.**
Because concurrency is knowingly unquantified at decision time, option 1 must be
activated only after the no-slice-CI queue baseline is recorded. The workflow
change being present on this implementation branch is not, by itself, the
activation boundary: for `pull_request`, GitHub evaluates the workflow from the
pull request's base branch. Do not use a commit's author or committer time as a
proxy for activation.

Before a branch containing this trigger becomes the base of any slice sub-PR,
capture `Q0` from 10 consecutive completed unrelated `pull_request` runs of
`ci.yml` while no evaluated base branch contains the slice trigger. An unrelated
run has a base branch other than `aimee/feat/**`. For each run, calculate queue
wait as the earliest job `started_at` minus the run `created_at`; `Q0` is the
arithmetic median (the mean of the fifth and sixth ordered values). Record the
run IDs, timestamps, per-run waits, API retrieval date, and resulting median in
this proposal. Do not activate slice CI until that record exists.

After `Q0` is recorded, merge or otherwise deploy the trigger to the branch that
will be the base of slice sub-PRs. Record the deployment event and its GitHub
timestamp as the activation boundary. Confirm activation with the first slice
sub-PR check run and record that run's ID and `created_at`; neither a local commit
time nor the implementation PR's creation time is an acceptable substitute.

For `Q1`, use the first 10 completed slice gates created at or after the recorded
activation boundary. Measure the queue wait of every unrelated `pull_request`
`ci.yml` run (base branch not `aimee/feat/**`) created from the first slice
gate's `created_at` through the last slice gate's `updated_at`, using the same
earliest-job-start calculation and arithmetic-median rule; `Q1` is the median of
those values. If the window contains fewer than 10 unrelated PR runs, extend its
end to the creation time of the tenth subsequent unrelated PR run and use those
10 runs for `Q1`.

For each slice gate, record its run ID, `created_at`, `updated_at`, total elapsed
time, and an account-wide concurrency maximum `Cmax[i]`. Record one API
observation cutoff after the gate's `updated_at`. The observation interval for
`Cmax[i]` is the gate's half-open interval `[created_at, updated_at)`. Using
credentials that can enumerate Actions across every repository owned by the
account, query every workflow run whose lifetime overlaps that interval, not
only runs in this repository, then retrieve every job attempt for those runs.
Count each execution (including every matrix expansion and rerun attempt) from
`started_at` to `completed_at`; if a started job has no `completed_at` at
retrieval time, count it through the recorded observation cutoff. Clip that
active interval to the slice-gate interval. Reconstruct the maximum with a sweep
of the clipped interval endpoints, processing completion endpoints before start
endpoints at an equal timestamp so the half-open convention is preserved.
Queued or skipped jobs without `started_at` are not active.

Preserve the raw API responses used for the calculation. Record the account
identifier, API retrieval date, account plan and documented concurrent-job
ceiling, repository and run IDs queried, each job attempt's ID and timestamps,
and the resulting ten `Cmax[i]` values so every maximum and overlap can be
reproduced. If the credentials cannot enumerate every account-owned repository,
the sample is incomplete and the concurrency criterion must not be evaluated.

Record the ten results as one row per slice gate with columns for run ID,
observation interval (`created_at`, `updated_at`), elapsed time, overlapping
account-wide repository and run IDs, `Cmax[i]`, and whether `Cmax[i]` equals the
recorded ceiling. The threshold count is the number of rows whose equality field
is true.

**Reconsider option 2 if any of these is true** (each mechanically checkable):

- `Q1 >= Q0 + 2 minutes`; **or**
- slice gate elapsed time exceeds **20 minutes** in **3 or more** of the 10 runs
  (against the measured ~10-minute standalone figure); **or**
- `Cmax[i]` reaches the account's documented concurrent-job ceiling for **2 or
  more** of the 10 slice gates. Apply this threshold to the ten per-gate maxima,
  not to one aggregate maximum. (The ceiling must be read from the account's
  plan limits at measurement time; it is not known here, which is precisely why
  it is recorded rather than assumed.)

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

`ci.yml` includes `'aimee/feat/**'` in the existing `pull_request` branch
filter. Once deployed to a branch used as the base of a work-item slice sub-PR,
that pull request starts the same workflow as pull requests targeting `main`,
`testing`, or `feature/core-modularization`. The implementation is intentionally
not activated until the pre-enablement `Q0` record required above is complete.
No job, matrix, permissions, event type, or workflow-engine CI semantics changed.

## Verification

1. Parse `.github/workflows/ci.yml` and confirm the `pull_request.branches`
   filter includes `aimee/feat/**` while retaining all previous target branches
   and pull-request activity types.
2. Compare the workflow before and after the implementation and confirm that
   every job definition and every matrix expansion present in `ci.yml` at
   implementation time remains unchanged; do not use historical hard-coded job
   counts as the gate.
3. After recording `Q0` and deliberately activating the trigger, open or
   synchronize a test pull request targeting an `aimee/feat/<work-item>` branch
   and confirm that all executions produced by the complete current workflow
   report check runs. Confirm a feature-to-`testing` pull request still produces
   that same complete gate.
