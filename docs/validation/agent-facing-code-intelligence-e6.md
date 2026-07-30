# Agent-facing code-intelligence E6 evaluation

- Date: 2026-07-30 UTC
- Pinned merged Aimee: `930bbe995502c9f584d895f4900b0e3562582030` (PR #2179)
- Corpus: `benchmarks/code-agent-effectiveness/e6-corpus.json`
- Prompt: `benchmarks/code-agent-effectiveness/prompts/e6-agent-task-v1.md`
- Raw record: `benchmarks/code-agent-effectiveness/results/e6-20260730.json`
- Summary: `benchmarks/code-agent-effectiveness/results/e6-20260730-summary.json`

## Result and decision

The deterministic retrieval layer has 16/16 eligible cells: ten answerable and six unanswerable.
Duplicate rate and scope leakage are both 0; precision, recall, abstention, and Python edge
precision/recall are all 1.0; retrieval p95 is 0.083 seconds; packet p95 is 466 tokens. These values
are a checked-in contract replay over the E0–E5 product fixtures and tests, not a substitute for
provider-backed coding outcomes.

The paired agent layer has 0 eligible cells in every arm. No fresh provider-backed
standard/observe/on/ceiling matrix was available at the pinned commit, so task success, confidence
bounds, efficiency, and pre-edit actuation have no denominator. The scorer therefore rejects
promotion and retains `code_context_mode=observe`. It does not splice the historical interrupted
97-artifact run or infer coding lift from retrieval tests. The bounded remaining paired experiment
is tracked by
[`agent-facing-code-intelligence-paired-evaluation`](../proposals/pending/agent-facing-code-intelligence-paired-evaluation.md).

## Reproduction

```bash
python3 -m unittest \
  benchmarks/tests/test_code_intelligence_e6_evaluate.py \
  benchmarks/tests/test_code_intelligence_checkpoint_runner.py
python3 benchmarks/code-agent-effectiveness/e6_evaluate.py \
  benchmarks/code-agent-effectiveness/results/e6-20260730.json
python3 benchmarks/code-agent-effectiveness/validate_fixtures.py
make -C src proposal-links-check
```

Twenty-two Python regressions passed. The fixture validator passed five inherited red/green contracts.
The E6 scorer emitted `retrieval_gate_pass:true`, `paired_agent_gate_pass:false`, and
`promotion_decision:retain-observe`. Background indexing and isolated rebuild cost are not reported
because there were no eligible paired cells; they remain mandatory in the residual matrix.

## Review

Acceptance roundtable run `oprun_g6a6b1ed93980fd5d_1785417195_27` requested three blocking
fail-closed repairs. The first scorer version could accept partial coding arms or retrieval cases and
defaulted missing duplicate/leakage fields to clean values. The scorer now requires the exact 16
corpus IDs and all eight task IDs in every arm, rejects duplicate/unknown/malformed rows, requires
explicit evidence fields, and cannot promote a partial eligible matrix. New regressions cover each
boundary; the corrected exact diff is reconvened below.

Round 2 (`oprun_g6a6b1ed93980fd5d_1785417454_28`, artifact
`51d2314b8a2b98e8a2b59e23068097726bb898c0ae0695e40ba8d0f3eee0e792`) found that eligible
coding fields had presence checks but not strict bool/finite-nonnegative type checks. The scorer now
validates coding decisions, token/wall metrics, retrieval timing/tokens, and Python edge rates before
any arithmetic. A malformed-metric regression is included in the next exact-diff review.

Round 3 (`oprun_g6a6b1ed93980fd5d_1785417816_29`, artifact
`fa1a46d6eb522bc9e398a6df49a79e68aae728e07f2c399e137649eda2a2f1d1`) approved the final exact
diff with 3/3 participants, no findings, and no degradation. Optional future hardening suggestions
did not change the promotion decision or accepted contract.
