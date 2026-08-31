# Agent-facing code-intelligence E6 evaluation

- Date: 2026-07-30 UTC
- Pinned merged Aimee: `930bbe995502c9f584d895f4900b0e3562582030` (PR #2179)
- Corpus: `benchmarks/code-agent-effectiveness/e6-corpus.json`
- Prompt: `benchmarks/code-agent-effectiveness/prompts/e6-agent-task-v1.md`
- Raw record: `benchmarks/code-agent-effectiveness/results/e6-20260730.json`
- Summary: `benchmarks/code-agent-effectiveness/results/e6-20260730-summary.json`
- Delivery: PR #2180 merged as `6969b2bca56f7d1d278e0796ce1213f40c3bad51` after 24/24 CI checks passed

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
[`agent-facing-code-intelligence-paired-evaluation`](../proposals/done/agent-facing-code-intelligence-paired-evaluation.md).

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

## Fresh provider-backed completion

The residual matrix subsequently ran from merged `testing` commit
`aa8c40e9d75449774c9b0b630bb8f1037efb8097` with Codex CLI 0.146.0,
`gpt-5.6-sol`, medium reasoning, and pinned prompt fixture v1. The final Aimee server and KB were
built from that checkout and deployed only in CT 331 on `.253`:

- server image `sha256:a7c2e51b7e338ef9a99d2dcb938252a85143f602f3d944fdb5bca7e322d63ed3`;
- KB image `sha256:42a952c664c0f166c1dbf8ac3700b6c253b838522873068523961ce31264c7d7`;
- stable fixture project `e6-aa8c40e-fixture`, current generation 1;
- raw cells `/home/virant/.local/share/aimee/e6-paired-aa8-20260730-v2/cell-artifacts`;
- checkpoint `/home/virant/.local/share/aimee/e6-paired-aa8-20260730-v2/checkpoint-run`.

The first attempted plan is preserved separately at
`/home/virant/.local/share/aimee/e6-paired-aa8-20260730`: it stopped with an
infrastructure-invalid symbol/corpus mapping and contributes no scored cells. The new v2 plan used
fresh artifact and checkpoint roots and completed 32/32 cells with zero exclusions or retries.

The deterministic retrieval contract record remains pinned to
`930bbe995502c9f584d895f4900b0e3562582030`. Reuse is explicit and bounded: the Git diff from that
commit to `aa8c40e9` changes only benchmark and documentation files, with no product source,
configuration, deployment, or test implementation changes. The 22 scorer/checkpoint regressions,
five contract fixtures, and proposal-link checks were replayed at `aa8c40e9`. The collector fails
closed on a commit mismatch unless this rationale is supplied, and the scorer independently rejects
an unacknowledged mismatch. That rationale is an operator attestation: it must identify the old and
new commits and state the inspected product-diff result; the tracked result preserves it verbatim.

| arm | success | 95% success LCB | median uncached input | median wall |
| --- | ---: | ---: | ---: | ---: |
| standard | 5/8 (62.5%) | 0.3057 | 33,626 | 78.24 s |
| observe | 5/8 (62.5%) | 0.3057 | 21,448 | 70.43 s |
| on | 6/8 (75.0%) | 0.4093 | 34,009 | 67.61 s |
| ceiling | 5/8 (62.5%) | 0.3057 | 29,423.5 | 66.18 s |

`on` improves median total wall by 13.59% versus standard, does not regress the lower confidence
bound, and consumes the packet before the decisive edit in 8/8 answerable cells. Its token median is
1.14% higher, so wall, not tokens, is the qualifying efficiency metric. Provider retrieval p95 is
1.562 seconds and packet p95 is 122 whitespace-estimated tokens, both within gate. A separate forced
34-file local index costs 1.241 seconds and the isolated KB rebuild costs 1.217 seconds, including
SSH/container dispatch overhead.

All eight `on` raw streams contain the exact `AIMEE-CONTEXT consumed` acknowledgement before the
first edit. The runner's parser requires that exact phrase rather than a generic context mention.

The generated summary records `required_arms_complete:true`, `paired_agent_gate_pass:true`, and
`promotion_decision:promote-on`. This authorizes changing the shipping default to
`code_context_mode=on`; explicit `off` and `observe` remain supported rollback modes.

Evidence checksums:

- cell-artifact manifest: `eddedd1705e47d33efb8ad9c38e57bf95960e807e7ec8c4ba2a27ce60c2ef1af`;
- checkpoint-tree manifest: `4495fd4be96d846728b852cc729e109e9b5b820defc90c86d59ec6989a642a0e`;
- plan: `b46b5693ebe05c2e48a24ae1d263d9468ce7da823250adb4d8b450b9fef44af0`;
- tracked provider result: `cb552a748c89d47105a673ad0c3513afffb23126883719794aef8b87ed1dc957`;
- tracked provider summary: `8048ec252e3a3c5679e90303d71b6fed588b6213c20c5520b7d3b05c1322d94d`.

## Provider completion review and merge

Acceptance run `oprun_g6a6b1ed93980fd5d_1785424107_32` approved the initial provider artifact and
identified portability/durability suggestions. Run `oprun_g6a6b1ed93980fd5d_1785424352_33` then
blocked an implicit retrieval-record commit mismatch. The collector and scorer were changed to fail
closed unless reuse carries an explicit operator attestation, with regression coverage. Corrected run
`oprun_g6a6b1ed93980fd5d_1785424759_34` approved the artifact, and final exact-diff run
`oprun_g6a6b1ed93980fd5d_1785425041_35` approved artifact
`db9a11ec81de2015b4bdbc43f374be1dc9a2ba10a8068997ccc2faa7eb93ea9e` with 3/3 participants,
no failures, and no blocking findings.

PR #2183 merged the provider evidence, default promotion, completed residual proposal, and operator
documentation to `testing` as merge commit
`e4bcba9034ce095a1752aa3037f5b214480ff669` after all 24 GitHub checks passed.
