# Paired exploration-contract release gate

`exploration_gate.py` evaluates MR-07 against the frozen MR-18 defaults. It
produces a review report and never enables serving enforcement. Its unit-test
cells are synthetic mathematical fixtures, not evidence of task success.

Before collection, commit an experiment manifest with `schema_version: 1`, the
exact `POLICY` object from the scorer, unique `ordered_case_ids`, and all `PINNED`
fields. Bind the corpus, model/tokenizer/index, ingestion/extractor, reader/judge,
seed, query class, final context budget, requested/effective candidate limits,
renderer, cache state and full cost components. Declare each arm's
`implementation`, `policy_sha256` and `feature_flags`. The sampling unit is
`independent_task`; do not count correlated retries or repeated runs of one task
as independent cases. Freeze any repetition aggregation before collection.

The results envelope repeats those pins and includes `manifest_sha256`, calculated
from the exact manifest bytes. `pairs` is in the same case order; every pair has
`case_id`, `baseline` and `treatment`. Each eligible arm records:

- `eligible`, `completed` and independently verified `correct` booleans;
- integer `raw_scans`, labeled `redundant_scans`, `restrictions`, labeled
  `false_restrictions`, and `expansions`;
- positive `latency_seconds`, nonnegative matched `total_cost`, and the SHA-256
  `evidence_sha256` of the retained raw collector/verifier evidence.

Keep expected answers and labels exclusively in evaluation. A missing label is
not zero. An invalid arm has `eligible: false` and `invalid_reason`; it prevents
promotion and is reported without silently shrinking the denominator. Keep the
raw evidence and verify its provenance during operator review: a supplied digest
is a commitment, not authentication of the collector or judge.

Run:

```sh
python3 benchmarks/memory/exploration_gate.py \
  --manifest frozen-manifest.json --results collected-pairs.json \
  --output new-report.json
```

Output paths must be unused. Exit zero means eligible for operator review; exit
one means remain in observe mode. Malformed or mismatched runs fail without
publishing a report. Reports bind both input files by hash.

The task-success difference counts success only when both completion and
correctness pass. Its conservative paired 95% interval combines exact 97.5%
Clopper-Pearson intervals for treatment-only and baseline-only successes using
Bonferroni bounds. This retains uncertainty for all-equal samples, including a
small sample with perfect success. Task pairs, not the two arms within a pair,
are assumed independent. A lower bound below -0.01 prevents promotion.

Other gates require at least 20% fewer redundant scans, treatment p95 latency
at most 1.10 times baseline, and no increase in matched total cost. P95 uses the
nearest-rank empirical quantile. A zero redundant-scan baseline cannot establish
an efficiency improvement. False-restriction rate is labeled false restrictions
over restrictions (null with no restrictions); expansion rate is expansions per
paired task. The scorer reports both arms and every gate, without treating these
secondary rates as an unmeasured safety claim.

Even a passing reviewed experiment is insufficient by itself: live enforcement
also requires operator opt-in, a supported query class, complete context,
authenticated task/scope binding, fresh index/worktree generations and a valid
calibration receipt. Unknown freshness continues to require observe mode.
