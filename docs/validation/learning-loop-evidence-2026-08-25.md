# Validation — six-loop learning evidence

This run turns the recursive-learning proof into one reproducible target and
closes the two live-path gaps left by the 2026-08-24 producer run. It exercises
both services and their modules against one throwaway PostgreSQL database, then
prints one evidence row for each of S1 through S6.

- **Branch:** `agent/learning-loop-ablation-arms`, based on `origin/testing` at
  `ace897e7a3`.
- **Environment:** `root@192.168.1.252` (pvetest), PostgreSQL 17.11, pgvector
  0.8.0, pg_trgm 1.6, Python 3.13.5.
- **Date:** 2026-08-25.

## Reproduction

From the repository root:

```sh
AIMEE_TEST_PG_URL=postgresql:///postgres make -C src learning-loop-evidence
```

The target builds the product and the three module hosts used by the harness,
creates a uniquely named scratch database, starts `aimee-kb` and `aimee-server`
with isolated homes, runs the evidence suite, stops every process, drops the
database, and removes the run directory.

Result on pvetest: **46 passed, 0 failed**.

```text
LOOP RESULT    OBSERVATION
S1   PASS      failed jobs -> admitted regression task
S2   PASS      paired-grid attribution (plumbing, not new-loop efficacy)
S3   PASS      failed approach -> recalled dead end
S4   PASS      evidence-backed curiosity close
S5   PASS      supersession and operator regret
S6   PASS      rewarded non-default policy arm
```

The wrapper's cleanup was checked after the run: PostgreSQL reported zero
databases matching `aimee_learning_evidence_%`.

## What each row proves

| Loop | Live observation |
| --- | --- |
| S1 | Two failed `agent_jobs` observations collapse into one candidate with occurrence count two; admission writes one task file and moves the ledger row to `admitted`. |
| S2 | The daemon reads three paired `full`/`no_rescue` tasks from PostgreSQL and reports `+1.000`, “removing it cost us.” |
| S3 | The S1 scan also stores one repeated dead end, and `aimee learning approaches` recalls its approach and failure mode. |
| S4 | An uncovered curiosity item remains open while an item covered by the memory graph becomes `resolved`; the pass reports the close. |
| S5 | A later commit supersedes the earlier proposal, and an operator verdict changes its fate to `contradicted` and counts as regret. |
| S6 | With exploration disabled and the `brief` posterior made overwhelmingly stronger than `off` and the default `full`, the live policy route selects and records `brief`. |

## Defects exposed by making the proof live

The old harness deployed a `db1` server module that no longer exists. DB1 is
owned by the `aimee` Go module, with the `aimee-db1` and `aimee-postgres`
grants authorising the same executable; the PostgreSQL module must also be
attached so the store schema is available. The evidence harness now follows
that production topology and waits for the store, rather than racing schema
application.

Once the real optimizer sidecar was enabled, S6 exposed a use-after-free in
`kb_bandit_sample`: it retained cJSON's `selected_arm` pointer, destroyed the
response, and only then compared the freed string. In the observed run the
sidecar selected `brief` but the service returned `off`. The code now copies
the arm id before destroying the response, and a focused unit test drives the
real sidecar and requires arm index 1.

## Claim boundary

S2 proves the live attribution path and its minimum-three-pairs guard. Its rows
are deliberately seeded and use the established `no_rescue` runner ablation.
They do **not** show that S1, S3, or S5 improve task outcomes.

The proposed `no_evalgrow`, `no_deadend`, and `no_supersede` arms have not been
added as labels to the agent runner: those loops execute outside that runner,
so such labels would not disable them and would manufacture a counterfactual.
The remaining efficacy work is a multi-phase benchmark whose setup and
consumer phases can genuinely omit each loop, run the same tasks and seeds,
and feed the resulting paired outcomes to the attribution ledger.

