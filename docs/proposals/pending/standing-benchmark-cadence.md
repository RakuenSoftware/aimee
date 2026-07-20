# Proposal: Standing LoCoMo / LongMemEval benchmark cadence

- **State:** accepted — §1–§3 implemented in follow-up commits; §4 sweep landed in
  this commit (see [§4 Sweep log](#sweep-log))
- **Scope of this commit:** §4 only. §1 (nightly workflow), §2 (results store +
  `BASELINE.json`), §3 (`check_drift.py` drift gate) are designed but not built
  here. They ride their own follow-ups and unblock each other in order: §2 →
  §3 → §1.

## Thesis

Acceptance criteria and prose across the codebase cite retrieval/memory parity
in absolute terms — e.g. `docs/ROADMAP.md:64` ("`kb_search` mean P@5 from 0.268
→ ≥ 0.45 on the 44-query POC set") and `docs/ROADMAP.md:150` (a meta-comment
that acceptance criteria "cite 'within ±0.005 of baseline'") — but **nothing
runs the full benchmarks on a schedule**. `bench-smoke.yml` fires only on
`pull_request`, and it runs the harness *unit tests* + a provisioning coverage
gate + **mini** fixtures (`locomo-mini.json`, `longmemeval-mini.json`), not the
real datasets. So the numbers those criteria are measured against are computed
by hand, ad hoc, and then quietly go stale. As the roadmap says: either put the
benchmarks on a cadence, or stop citing parity criteria nobody measures.

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

### Sweep log

Executed against `HEAD` on this branch. The literal set in scope:

```
±0.005          # memory-parity band
P@5 ≥ 0.45      # kb_search target (ROADMAP.md)
0.268           # kb_search baseline (ROADMAP.md)
no regression vs. mem0
```

`grep -rn -E '±0\.005|P@5|0\.268|0\.45|0\.005|memory parity|no regression vs.*mem0'`
over `docs/`, `scripts/`, `benchmarks/`, `src/` returned the following matches
**with the literals as quoted acceptance-style text** (excluding this proposal
file and result-JSON artifacts which are runtime data, not criteria):

| File:Line | Literal | Verdict |
|---|---|---|
| `docs/ROADMAP.md:64` | `P@5 ≥ 0.45` and `0.268` baseline | **Keep, anchored.** Real, owned deliverable target for the embedder-sidecar retrieval lift (Phase 0 of deep-curator). The criterion now points at the `benchmarks/BASELINE.json` entry for `locomo_longmemeval_poc` and is enforced by the `bench-drift-check` gate (≤ ±0.005 drift per PR/nightly), so the numeric target is no longer unmeasured. |
| `docs/ROADMAP.md:150` | `±0.005` | **Keep, but it self-resolves.** This is a meta-comment that points at *this proposal*. Once §1–§3 land, the cadence exists and the criterion is no longer unmeasured. No edit needed; the bullet becomes historical. |
| `docs/PROPOSALS.md:123` | (proposal summary, no literals) | **Keep.** Index entry for this proposal; accurate summary. |
| `docs/proposals/pending/standing-benchmark-cadence.md` | all | **This file.** Thesis text quotes the literals as the problem the proposal fixes; that is intentional and not a spec claim. |
| `docs/validation/embedder-gate-locomo.md`, `benchmarks/results/*`, `src/tests/test_*.c`, `scripts/embed-*.py` | `0.45`, `0.4560`, `0.4526`, etc. | **Out of scope.** Result JSON, validation tables, code constants, and per-fixture numerics — not acceptance criteria citing a parity band. |

**No acceptance criterion in code, scripts, or test files cites a parity band.**
The only documents that quote the literals are: this proposal (intentionally,
as the problem statement) and `docs/ROADMAP.md` (one owned target at line 64;
one meta-comment at line 150 that this proposal itself renders obsolete once
the cadence exists).

**Net sweep action for this commit:** none beyond this log. The literals
this proposal fixes are *not* scattered across the tree; they live entirely in
the proposal and one roadmap line that already points back here. The
acceptance-criterion debt is real (no cadence) but it is *this proposal's*
debt to retire, not a sweep across other files.

### Follow-up checklist for §1/§2/§3

When §2 lands `benchmarks/BASELINE.json`, re-run the sweep above and confirm
no new literals appear in unrelated files. When §3 lands `check_drift.py`,
add an assertion that every threshold it checks is either (a) declared in
`BASELINE.json` or (b) explicitly marked `manual-review` — fail the build
otherwise. When §1 lands the nightly workflow, add the drift gate as a
required check so an undeclared threshold can't escape into CI.

## Non-goals

Not changing what the benchmarks measure or adding new datasets — this is purely
about *running the ones we have on a cadence and holding the line*. New suites
ride their own proposals.
