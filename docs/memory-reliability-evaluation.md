# Memory reliability evaluation and promotion

Run `python3 scripts/check_memory_reliability_release.py --output RESULT.json`
with disposable `AIMEE_MEMORY_EVAL_URL`, `AIMEE_DB2_REPLAY_URL`,
`AIMEE_TEST_PG_URL` and `AIMEE_DB_TEST_URL` databases. Bootstrap the DB2 replay
store from `src/modules/db2/c/schema.sql`. Use distinct DB1/DB2 stores, a role able
to create disposable evaluation databases, and serialize their test runners.
The required `memory-retrieval-eval` CI job provisions these stores.

`tests/eval/memory_reliability_release_v1.json` freezes twelve adversarial groups,
ordered test identities, source/fixture hashes and promotion thresholds. It
references the original 123 acceptance clauses without changing their text or
hashes. Tests contain success and refusal variants. A changed fixture requires
explicit review and a new recorded hash; the gate never refreshes hashes itself.
A required skipped test or subtest, missing test, failed package or malformed
Go test stream rejects the run. A pass artifact is written atomically only after
all required cases execute. Its implementation commit and working-diff digest
must match the checkout under review. Zero violations means zero in these
fixtures, not a guarantee of zero production risk.

The Python LoCoMo/LongMemEval readers retain questions without gold evidence.
Every input question receives an ordered ID and inclusion/exclusion reason,
including caps and malformed empty-question/history rows. Duplicate IDs fail.
A failed reader/judge run publishes no successful result. Result inventories
reject missing, reordered or duplicated cases; explicit timeout, invalid and
infrastructure-error statuses remain in the denominator. Go dataset runs also
retain unanswerable/unlabelled cases, record caps by ID and publish their raw
retrieval receipts. Relevance scores use only labelled, answerable cases;
undefined metrics are null, never perfect zero/one. Dataset IDs in Go are scoped
by sample index so reused local question IDs remain distinct.

LongMemEval's `_abs` marker follows its
[official evaluator](https://github.com/xiaowu0162/LongMemEval/blob/main/src/evaluation/evaluate_qa.py).
LoCoMo's explicit category 5 follows its
[official evaluation code](https://github.com/snap-research/locomo/blob/main/task_eval/evaluation.py).
Question wording is never a gold answerability label. Gold answer text and IDs
are supplied only to scorers/judges; the reader receives question and retrieved
context, and the serving planner receives no gold labels. Regex/text abstention
detection is explicitly labelled as a heuristic. The confusion matrix,
precision/recall, unsupported-unanswerable-answer rate and observed risk/coverage
point are not substitutes for reviewed citation support or a confidence curve.
No abstentions makes precision undefined. The separate MR-05 frozen coverage
fixture measures reviewed requirement satisfaction and false-complete behavior.

Retrieval MRR measures first relevant rank; recall is the fraction of labelled
fixture evidence retrieved. nDCG uses binary fixture membership. These are not
end-to-end answer correctness, full required-evidence satisfaction or task cost.
Owner retrieval latency is not a Server-to-KB round trip. The original 105-case
baseline remains immutable: changed schema/policy/corpus identities require a
new separately recorded experiment, not reuse of historical measurements.

For paired task promotion, freeze a version-1 manifest accepted by
`benchmarks/common/reliability_experiment.py` before running either arm, then run:

```sh
python3 scripts/evaluate_memory_promotion.py \
  --manifest frozen-experiment.json --baseline baseline.json \
  --candidate candidate.json --output decision.json
```

The manifest pins ordered IDs, answerability, implementation, dataset/corpus
hashes, evidence unit, model/tokenizer, index generations, ingestion/extractor,
policy/flags, reader/judge, seed, requested/effective candidate limits and final
payload budget. Protocol-compatible and product-optimized tracks are separate.
Both run artifacts must bind that exact manifest digest and arm. Each ordered
case includes explicit status, reviewed success, total stage cost, latency and
raw discovery count. Label token-rate cost as `estimated-token-rates`, include
the frozen rate model, and use the same accounting in both arms. Missing or
invalid cost is not zero. Billing cost and byte-based retry reservations are
separate quantities.

Version 1 requires at least 30 complete pairs; it uses 10,000 paired bootstrap
resamples with the frozen seed and reports the 95% interval for success change.
The lower endpoint must be at least -0.01; candidate p95 latency must be at most
1.10 times baseline. An efficiency claim additionally requires no increase in
matched total cost; exploration promotion requires at least 20% fewer redundant
discovery calls. Missing/invalid attempts or insufficient pairs are unqualified.
These thresholds are decision rules, not measured benefits. Degenerate samples
and judge bias remain limitations of the interval; held-out tasks, repetitions,
reader/judge sensitivity and dependency-ordered ablations belong in each real
experiment report. The evaluator never changes runtime policy.

MR-07 remains observe-only by user direction. MR-09 adaptive selection and MR-10
utility horizons remain disabled; MR-17 clean retry remains opt-in/disabled in
the canary. Existing small paired studies are retained under their original
identities and are not relabelled as passing this newer gate. A failed optional
promotion does not disable eligibility, authorization, versioning or receipts.
