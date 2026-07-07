# Proposal: Standing LoCoMo / LongMemEval benchmark cadence

- **State:** proposed (pending — not started)

## Thesis

Acceptance criteria across the codebase cite retrieval/memory parity in absolute
terms — "within ±0.005 of baseline", "P@5 ≥ 0.45", "no regression vs. mem0" —
but **nothing runs the full benchmarks on a schedule**. `bench-smoke.yml` fires
only on `pull_request`, and it runs the harness *unit tests* + a provisioning
coverage gate + **mini** fixtures (`locomo-mini.json`, `longmemeval-mini.json`),
not the real datasets. So the numbers those criteria are measured against are
computed by hand, ad hoc, and then quietly go stale. As the roadmap says: either
put the benchmarks on a cadence, or stop citing parity criteria nobody measures.

## Goal

A scheduled job runs the **full** LoCoMo + LongMemEval (and the memory /
code-vector suites) against `testing` on a fixed cadence, persists the scored
results with a retention policy, and fails loudly on a drift beyond the parity
band that acceptance criteria already cite — so "±0.005 of baseline" becomes a
measured, enforceable fact instead of a hope.

## §0 What already exists

- **Harness is complete.** `benchmarks/locomo/bench_aimee_direct.py`,
  `bench_aimee_llm.py`, and the comparison baselines (`bench_bm25_llm.py`,
  `bench_mem0_llm.py`, `bench_rag_chromadb_llm.py`); `benchmarks/longmemeval/`;
  `benchmarks/memory/`; `benchmarks/code-vector-graph/`. Results docs
  (`BENCHMARK_RESULTS.md`, `EVAL_CONFIG.md`) exist per suite.
- **Make targets exist:** `bench`, `bench-check`, `bench-baseline`,
  `memory-retrieval-eval-check`, `curator-eval-check`.
- **CI runs none of the full datasets.** `.github/workflows/bench-smoke.yml` is
  PR-triggered and mini-only; `benchmarks/check_provisioning.py` gates coverage
  but scores nothing.

The pieces to *run* a benchmark exist; the missing thing is a scheduler, a
results store, and a drift gate.

## §1 Scheduled full-suite workflow

Add `.github/workflows/bench-nightly.yml` (`on: schedule:` — nightly for the
cheap suites, weekly for the LLM-graded ones, plus `workflow_dispatch`). It
provisions the real datasets (gated behind a repo secret / self-hosted runner
where the datasets or an LLM endpoint are needed), runs the full `bench_*`
scripts, and writes scored JSON. Keep it OFF the PR path — PRs keep the fast
smoke; the full run is a cadence, not a blocker.

## §2 Results store + retention

Persist each run's scored JSON as a dated artifact (retention policy, e.g. 90
days for raw runs, indefinitely for the committed baseline). Commit a single
rolling `benchmarks/BASELINE.json` (per suite, per metric) that the parity checks
read, updated only by an explicit "accept new baseline" step — never silently by
a run.

## §3 Drift gate wired to existing criteria

A `benchmarks/check_drift.py` compares the latest scored run against
`BASELINE.json` and fails when any cited metric moves beyond its declared band
(P@5, LoCoMo/LongMemEval accuracy, memory parity ±0.005). Wire it into the
nightly workflow and expose it as a `bench-drift-check` make target so it can run
locally. Acceptance criteria stop citing an unmeasured number.

## §4 Make the criteria honest

Sweep the acceptance criteria that cite a parity band and either (a) point them at
`BASELINE.json` + the drift gate, or (b) delete the citation. A criterion that
references a number no job measures is debt, not a spec.

## Non-goals

Not changing what the benchmarks measure or adding new datasets — this is purely
about *running the ones we have on a cadence and holding the line*. New suites
ride their own proposals.
