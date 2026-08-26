# Self-learning efficacy paired study, 2026-08-26

## Result

On `pvetest`, the paired repeated-task score was **12/24 with synthesis
withheld** and **24/24 with self-learning enabled**. The novel-task control was
**12/24 in both conditions**. There were 12 treatment-only successes and no
control-only successes (exact two-sided McNemar p = 0.00048828125).

The study was repeated against a second fresh PostgreSQL database. Both valid
runs passed 12 harness checks with no failures and produced byte-identical
`results.csv` files:

```text
250a617ff71ad3f069fdd5bd9c82ebc142f3e694693fb368c971706abafaf62c
```

## Question and design

The study asks whether Aimee's learned record of a failed approach changes a
later outcome when the task and starting choice are held fixed.

- Both conditions received the same 48 failed `agent_jobs`: two observations
  for each of 24 repeated tasks.
- The control withheld the production synthesis pass before its consumer
  phase.
- The treatment ran `aimee eval candidates-update scan`, then read the result
  through the production `aimee learning approaches` command.
- A deterministic consumer began with the same fixed first choice in each
  condition. It changed that choice only when the production recall output
  identified it as a failed approach.
- Another 24 tasks had no matching history. They tested whether unrelated
  learned records changed the result.

The run started `aimee-kb`, `aimee-server`, and their required config,
learning, memory, and PostgreSQL modules. Each condition used the same binaries
and a freshly reset database state.

## Scores

| task class | synthesis withheld | self-learning enabled |
|---|---:|---:|
| repeated tasks | 12/24 | 24/24 |
| novel tasks | 12/24 | 12/24 |

All 24 treatment advisories contained the matching failed approach. Neither
condition produced an advisory for the novel tasks. Treatment stored 24 failed
approaches and retained both observations for each, for 48 observations in
total.

## Environment

- Commit: `ceea316ca12ad2f49ecc7ea9842e00701a6b7300`
- Host: `pvetest`, Linux 7.0.14-8-pve x86_64, 8 cores, 31 GiB RAM
- PostgreSQL 17.11
- Python 3.13.5

Command:

```sh
AIMEE_STUDY_OUTPUT=/path/to/fresh/output \
  AIMEE_TEST_PG_URL=postgresql:///postgres \
  make -C src self-learning-efficacy
```

The runner creates and drops a fresh database. The two valid output directories
were `run-20260826T1425Z` and `run-20260826T1344Z`. They contain the cell-level
CSV, JSON summary, all 96 recall outputs, synthesis output, environment record,
service and module logs, and harness totals.

## Invalid attempts retained

Three earlier output directories remain on the test host and are excluded from
the result:

1. `run-20260826T1328Z` stopped before setup because the host lacked the
   requested PostgreSQL role.
2. `run-20260826T1416Z` exposed a readiness probe that could terminate under
   `pipefail`; no consumer cases ran.
3. `run-20260826T1420Z` produced the same cell-level scores as the valid runs,
   but three harness assertions failed because their awk strings were escaped
   incorrectly. It is not counted as a valid run.

## Claim boundary

This is a controlled test of the deployed synthesis and recall path, not a
benchmark of model reasoning. The fixed consumer isolates the effect of
recalled failure on a later choice. The result does not estimate how often a
model will follow the same advisory during open-ended work, nor does it measure
generalisation beyond matching task descriptions.
