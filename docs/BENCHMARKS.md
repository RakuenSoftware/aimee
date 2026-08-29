# Benchmarks

A benchmark result is useful only with its corpus, build, model, configuration, hardware, and command.
Checked-in results under `benchmarks/` preserve those conditions; they are not universal product
claims.

## Suites

| Area | Measures |
| --- | --- |
| memory | recall quality, contradiction, temporal and long-context behavior |
| retrieval | lexical/dense/graph fusion, abstention, citation |
| curator | extraction, typed artifacts, contradiction and queue throughput |
| code graph | symbol/caller/blast-radius precision across repositories |
| delegates | task success, tool use, cost, latency, and failure class |
| guardrails | allow/deny quality and false positives |
| inference | embed/synthesis quality, slots, memory, latency |
| event bus | dispatch overhead, flow control, capture, durability, shutdown |
| provisioning | clean install, readiness, recovery, and upgrade |

## Event-bus gate

The bus has committed dispatch and audit-enqueue baselines and ceilings. Dispatch measures roughly
134 ns per event against a 1,000 ns ceiling for host enqueue through client dequeue, excluding
module work. Governed-action audit enqueue has the same 1,000 ns ceiling.
Its controlled reference measurement is 117 ns: the median of eight per-run
medians, with each run timing 5,000 emits and the test pinned to one CPU.
Run the gate on comparable hardware and keep the raw artifact. A slower machine can still be
correct; changing the committed ceiling needs an explicit decision, not a convenient rebaseline.

Durability, replay, retention, shutdown, and C/Go conformance are correctness gates, not latency
percentiles.

## Run

```bash
make -C src bench
make -C src bench-check
make -C src memory-retrieval-eval-check
make -C src curator-eval-check
```

Individual suites have their own README and fixtures. Live provider and KB suites should skip with a
clear reason when the dependency is absent; they must not report a pass from an empty run.

## Report

Record:

- commit and dirty state;
- command and exit status;
- host CPU/GPU/RAM/kernel/runtime;
- corpus name, version, size, and hash;
- model IDs, digests, dimensions, quantization, and slots;
- resolved configuration and feature gates;
- warmup, repetitions, timeouts, and concurrency;
- quality metrics with sample counts or uncertainty;
- latency distribution, not only mean;
- failures, skips, and degraded paths.

Keep raw machine output beside the summary when practical.

### Coverage is recorded, not remembered

Every result file the memory and retrieval producers write carries a `coverage`
block stating the caps that were in force (`--max-samples` / `--max-cases` /
`--max-questions`) and how much actually ran. A run with no cap is `complete`.

A subsampled run and a full run are otherwise the same shape, so a score from a
short run can be read back as a reproduction of a long one. That has already
cost us once: reranking measured **+0.020** on a 600-question subsample and
**-0.0048** on the full 10,000, a sign flip, and the recommendation was nearly
shipped on the subsample. See
[the retrieval writeup](blog/we-measured-our-reranker-and-deleted-it.md).

`benchmarks/verify_scores.py` prints coverage for every file, and
`--require-complete` fails the run unless every file proves it was uncapped. Use
it wherever a score is promoted rather than merely read: baseline eligibility,
cross-run comparison, and published claims. A file with no coverage block is
refused there too: an unknown question count is not evidence of a full run.

## Compare

Change one variable at a time. Compare against the same corpus and acceptance metric. A cheaper or
faster configuration is a win only if quality stays above the declared gate.

Do not compare a local measured score directly with a paper score that used another corpus,
prompt, judge, or retrieval budget. Reproduce the baseline in the same harness first.

## Rebaseline

Rebaseline only after an intentional contract, dependency, model, or hardware change. Preserve the
old artifact, explain the delta, and make the acceptance decision reviewable.

Code-intelligence matrix execution and resume semantics are documented in
[`benchmarks/code-agent-effectiveness/README.md`](../benchmarks/code-agent-effectiveness/README.md).
In particular, infrastructure failures are preserved but excluded from scoring, and
named checkpoints are bound to one immutable run and plan to prevent result splicing.

The E6 promotion harness is repository-owned and fail-closed. Its corpus, prompt fixture, raw result
envelope, generated summary, and scorer are versioned together. Deterministic retrieval success
alone cannot promote task context: missing or infrastructure-invalid paired agent cells are reported
outside denominators and force `retain-observe`.
