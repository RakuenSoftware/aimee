# Go retrieval compatibility decisions

These are intentional serving contracts for the Go cutover, not a claim that the
new rank order reproduces the retired native engine. The C bus remains C.

| Area | Decision | Evidence / migration consequence |
|---|---|---|
| Query expansion | Retire the legacy benchmark question normalization/truncation and implicit native semantic expansion. The owner receives the original query; its ordinary lexical parser and embedding input bounds still apply. No benchmark-only query rewriting or gold injection. Server search in both placements preserves queries up to 16,384 bytes and rejects longer ones instead of silently cutting at 2,047 bytes. | `dataset_test.go`, `corpus_test.go`, `label_audit_test.go`, `public_commands_test.go` and restricted-role Server search replay; artifact fixture policy `full-text-raw-query-v1`. Historical native scores need paired remeasurement. |
| Candidate admission | Keep scoped lexical plus active-version whole-record/unit/temporal semantic recall; apply visibility, lifecycle, suppression and current-input checks before limits. | Packaged-role `shared_recall_test.go` and `unit_recall_test.go`, including a matching unversioned vector that must not be admitted. |
| Unversioned vectors | Do not infer model compatibility from vector width. Legacy vector rows are retained for maintenance, but they do not supply this semantic lane. Re-embed and activate a named version with a verified serving identity. | `reembed_test.go` and `shared_recall_test.go`. Lexical recall remains available on ordinary dependency failure; evaluation explicitly refuses semantic fallback. |
| Rank combination | Keep Go reciprocal-rank fusion with k=60. Deduplicate each arm before allocating its ranks; distinct-arm agreement still contributes. Retire the unused C interleaving fixture and native effective-importance formula. Confidence is not a relevance multiplier. | `candidate_fusion_test.go` and owner diagnostic tests. Duplicate copies cannot inflate an arm's score. No equality with the historical weighted C ranker is asserted. |
| PageRank | Retain bounded Go PageRank behind its existing opt-in setting. Convert its bounded contribution into RRF units under `rrf60-pagerank-v1`. | Kernel golden fixtures, owner recall tests and postcommit timing regressions. Microbenchmarks do not replace end-to-end latency measurements. |
| Calibration | Offline diagnostic multipliers operate on one frozen candidate set per case. They are not a serving policy and cannot be applied as native configuration. | `label_audit_test.go`; artifact `diagnostic-multipliers-v1`, `deployable: false`. |

The initial frozen input is `tests/eval/memory_retrieval_manifest_v1.json`, checked
against the complete corpus bytes and ordered case IDs by the Go evaluator tests.
Generated corpus baselines additionally bind semantic corpus content, the actual
schema snapshot, embedding identity/dimension and effective ranking configuration.
An old baseline with only aggregate metrics is unbound and cannot pass this gate;
creating a replacement requires explicit `--update-baseline` after a successful run.

Still required: real-provider paired quality/performance, the complete adversarial
manifest and transport/restart/failure matrix, pinned temporal anchors and all
MR-18 release gates. Command embedder identity binds its configured command; it
does not independently attest every executable or model-weight file that command
may read. This limitation must not be described as full reproducibility.

Live benchmark compatibility: CLI and Server now share the Go live owner. Invalid
corpus rows/duplicate labels refuse the run instead of being skipped or truncated.
Unlabelled queries are excluded from the quality denominator and retained in
latency measurements. Every retrieval must succeed. Live metrics measure owner
latency and cannot be compared directly to the old Server-to-KB hop timings.
