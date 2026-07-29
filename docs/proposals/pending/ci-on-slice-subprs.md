# Proposal: run CI on slice sub-PRs

- **State:** approved — single slice.

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

- `ci.yml`'s `pull_request.branches` list does not include `aimee/feat/**`.
- PR #2011 merged with `check-runs` total_count = 0.
- `GIT_PR_CI_NONE` is defined at `src/modules/git/git_pr_api.h:75`
  (`GIT_PR_CI_NONE = 0, /* no CI reported for the head commit */`), and
  `src/modules/workflows/wfe_live_forge.c:197` handles that case with the
  rationale recorded in the comment at line 201 — the engine "already treats
  `GIT_PR_CI_NONE` as merge-permitting". This is verified design intent, cited to
  file and line, not inferred from behaviour.
- **`RakuenSoftware/aimee` is a public repository** (`gh repo view`:
  `"visibility":"PUBLIC"`). GitHub-hosted *standard* runners are free for public
  repositories; the per-OS billing multipliers (Linux 1x, Windows 2x, macOS 10x)
  apply to private-repo minute consumption, not here.
- **Job structure:** `ci.yml` defines **15 jobs**. One of them, `e2e-docker`,
  carries `matrix: topology: [T1, T2, T3]` and expands to 3 executions, giving
  **17 job executions per run**. Every job targets `ubuntu-latest`,
  `windows-latest` or `macos-latest` — all standard hosted runners, all free-tier
  for this repo. The table below and the concurrency figures are stated in
  *executions* (17), not definitions (15), since executions are what consume
  runner slots.

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
2. **Runner concurrency.** A 5-slice run adds 5 x 17 = **85 job executions**
   (17 executions per run, see Job structure above) competing for the account's
   concurrent-job allowance against all other CI in flight. This is the one
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
carries an obligation to measure it afterwards rather than leave it open:

Baseline first: before enabling slice CI, record the median queue wait of PR
runs over 10 runs; call it `Q0`. Then, over the first 10 pipeline runs with slice
CI enabled, record: maximum concurrent job executions reached (`Cmax`); median
queue wait of *unrelated* PR runs (`Q1`); and total elapsed time of each slice
gate.

**Reconsider option 2 if any of these is true** (each mechanically checkable):

- `Q1 >= Q0 + 2 minutes`, measured as the median over those 10 runs; **or**
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
