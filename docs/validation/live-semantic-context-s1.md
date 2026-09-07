# Validation: live semantic context S1

S1 passes its preregistered material-value gate on evidence epoch 3. This result promotes the
narrow local, saved-file, definition/reference context operation for S2 productionization. It does
not authorize detached routing, new semantic methods, opaque handles, dependency traversal, or a
second durable index.

## Immutable lineage

- Candidate commit: `474bd69954237fca249eb44e942caeab4270ad5e`
- Candidate `src` tree: `e6ba59ceba5a40323b006e834ac28ef39a2abc46`
- Runner commit: `682bd5c55ff4ba6b4b131de7debef2af5e906321`
- Experiment contract SHA-256: `345610b19bad3644260d731dea1ffd225971d74b57c26f041a2d55f2cda1bee1`
- Task manifest SHA-256: `612e64f133ed338c30fff51d52f10823dc22ee3fa5905894a1d38b087fa5dae1`
- Paired run: `live-semantic-s1-1d7148b02e744355`
- Adversarial run: `live-semantic-s1-44e8c4027cac486e`

The checked evidence is under
[`benchmarks/results/live-semantic-context/s1-epoch3`](../../benchmarks/results/live-semantic-context/s1-epoch3).
The byte-level directory hashes, including all raw model and tool JSONL, are:

| Evidence | Files | Tree SHA-256 |
| --- | ---: | --- |
| Paired value | 273 | `78c89fd52214679ed179a582b253bce257ccdc9970e73cadb3c0cf5a852a8b03` |
| Adversarial failure | 27 | `c0b4f30eeb092b3632c84310d118377ce1df9fdef5f0d5db802b1713f1e85b57` |
| Linux/macOS providers | 2 | `619bbe11d9d1b23b8452f64f52bbb6d4b3a78d29b2ff7e9bfb2974b7149c1703` |

`test_live_semantic_context_s1_evidence.py` recomputes those hashes and rejects missing, changed,
ineligible, or incomplete evidence in the PR benchmark gate.

## Paired value result

All 135 cells completed and were eligible; no infrastructure rerun occurred. On the 30
semantic-eligible paired tasks, batched context passed the efficiency branch of the material-value
gate:

| Metric, batched context versus production | Estimate | Paired 95% interval |
| --- | ---: | ---: |
| Task-success absolute delta | +3.33 points | 0.00 to +10.00 points |
| Median tool-call reduction | 66.67% | 50.00% to 66.67% |
| Median wall-time reduction | 38.69% | 33.27% to 41.80% |
| Median input-token reduction, descriptive only | 55.12% | 46.32% to 57.11% |

The success-improvement branch does not pass because its interval includes zero. The independent
efficiency branch passes: task success held, and both tool calls and end-to-end wall time exceeded
their 20% and 10% thresholds with intervals above zero. Token reduction is reported but was not an
acceptance criterion.

Across all 45 tasks, batched context was exact on 45/45, production on 44/45, and shipping
location-only on 22/45. Candidate adoption before decisive evidence was 29/30 (96.67%), above the
80% gate. The 15 controls were exact in every arm; candidate versus production had zero success
delta, zero median tool-call change, and a 1.45% median wall-time reduction. The control gate
therefore passes without treating control avoidance as failed adoption.

## Safety, freshness, reference quality, and cost

The separate 12-cell LSP adversarial run preserved all typed missing, unsupported, timeout, crash,
stale, and path-escape outcomes. It recorded zero false-current results, zero false `ok` empties,
zero authority-citation failures, and zero infrastructure failures.

GitHub Actions run `33540839136` exercised 20 clean starts per provider on each claimed platform:

| Platform | Provider | Clean starts | Reference recall | Reference false-positive rate | Peak process tree | Peak RSS KiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Linux | gopls | 20/20 | 100% | 0% | 3 | 786900 |
| Linux | Pyright | 20/20 | 100% | 0% | 3 | 732556 |
| macOS | gopls | 20/20 | 100% | 0% | 5 | 767152 |
| macOS | Pyright | 20/20 | 100% | 0% | 3 | 734464 |

The retained CI artifacts are `9813670808` (Linux, digest
`sha256:f7a7ef32b57779a38d0350218ab1699fcb4fc773611ff1bc0fe7f0b2c96df1db`) and
`9813661207` (macOS, digest
`sha256:9bfb855c8d551347fca5f4451b345b731b8b65ca62a75ee589bb214fd8eb8fd5`).
Both name the exact candidate commit and source tree above.

## Critical interpretation

This is a real value add for the measured operation: exact semantic localization plus bounded
current source reduces agentic tool round trips and total time while preserving correctness and
authority. It does not show that a new persistent context engine, opaque handles, or broad IDE
surface would add value. The tasks are read-only localization tasks, not edited-diff outcomes, so
the result supports selective semantic investigation and source retrieval. Any claim about edit
quality requires a separate checked change-task experiment.

S2 may now productionize only the passing local workflow: readiness/setup, selective routing for
the measured task classes, process sharing, observe/on/rollback controls, and continued comparison
against ordinary local inspection. S3 and S4 remain gated independently.
