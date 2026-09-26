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

## Reviewed deployment artifact

The host reads only `/etc/aimee/exploration-calibration.json`, with explicit
`AIMEE_EXPLORATION_ENFORCE=1` opt-in. The file and its ancestors must be root-owned
and not group/world writable; symlinks are rejected. It is never loaded from a
repository, tool arguments, or an agent-created policy file. No artifact is
included in the release, and the parser's synthetic tests must not be used as
measurements.

The JSON envelope contains `schema_version: 1`, nonempty `reviewed_by`, RFC3339
`created`/`expires` (at most 30 days apart), `scope`, `limits`, `report_sha256`,
and the complete scorer `report`. Hash the exact compact JSON representation of
`report` before embedding it. The report retains the frozen manifest and raw
results hashes. Review must verify the underlying collector/judge evidence;
aggregate numbers alone cannot attest task quality.

`scope` pins `project`, `workspace`, `working_directory`, `worktree_generation`,
`index_generation`, `query_class`, `route`, `provider`, `model`, and
`limits_digest`, and `producer_build`. Values must exactly match the issued live contract; wildcards
are unsupported. `limits` must exactly match the host's adaptive policy. This
first activation path accepts only `enabled: true` with an explicit `raw_scans`
ceiling and optional `starvation_turns`; file/graph/byte/token caps do not have a
calibrated live accounting path yet.

The host requires complete Go-owned coverage, current index and memory-owner
observations, a clean pinned checkout and a final provider receipt. It checks the
scorer's policy, decision, interval and all numerical gates. Each admission
re-reads deployment approval; removing the artifact or opt-in immediately stops
new adaptive restrictions while preserving operator limits and usage. Repeating
an already admitted attempt retains its original accounting decision.

Implementation status: activation parsing and native freshness checks are
implemented; no measured passing workload has been collected. Native requirement forwarding and hook freshness admission are implemented.
Fresh process acceptance and measured workload acceptance remain required before
MR-07 completion.

## Collection before calibration

A bounded experiment can collect treatment observations before a passing report
exists. It is a separate authorization, not a calibration and not a promotion.
Use only a disposable evaluation deployment. No experiment artifact is shipped.

Set `AIMEE_EXPLORATION_EXPERIMENT` to the exact frozen manifest SHA-256 and install
`/etc/aimee/exploration-experiment.json` through the deployment owner. The same
root ownership, protected ancestors, no-symlink and 64 KiB bounds apply. Its fields
are `schema_version: 1`, `kind: "experiment"`, `authorized_by`, `created`,
`expires` (at most six hours), `manifest_sha256`, exact `principal`, `sessions`
(one to 512 unique pre-created session IDs), `scope`, and `limits`. Scope and
limits use the calibration schema above. Wildcards cannot authorize sessions.
An experiment still requires complete Go coverage, current index and owner,
clean host worktree, final receipt and matching provider/build/budgets.

Contracts retain `approval_kind: "experiment"` and a tagged
`calibration_receipt: "experiment:<manifest-sha256>"`. This field is a durable
approval commitment; the experiment tag explicitly means no passing calibration
exists. The calibration parser rejects an experiment envelope. Experimental
opt-in takes precedence: a missing, invalid, expired or revoked experiment
falls back to observe, without silently falling through to a release approval.
Removing both opt-ins leaves observe mode and preserves all accounting.

Freeze separate baseline/treatment sessions before collection. The baseline has
no experiment opt-in. Record both raw provider usage and all declared stage cost
components; unavailable usage/cost is an invalid cell, never an estimated zero.
Experimental authorization alone is not evidence that the task-quality gates
passed. The measured report still decides whether release review is eligible.
