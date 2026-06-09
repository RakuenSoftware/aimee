# Proposal: generalise the `memory.benchmark` RPC beyond code-graph-fusion

- **State:** done
- **Author:** JBailes
- **Date:** 2026-06-08
- **Completed:** 2026-06-09
- **Charter role(s):** evaluation / benchmark surface (kb-backed retrieval eval;
  no new store, no new DB tier).

## Shipped

`memory.benchmark` now dispatches on `suite` instead of hard-rejecting every
suite except `code-graph-fusion`.

- `code-graph-fusion` keeps the existing committed production-corpus path and
  live KB retrieval scoring.
- `memory`, `corpus`, `memory-retrieval`, and `live` use the KB-provided eval
  corpus via `kb_client_memory_load_eval_corpus`, run live KB retrieval through
  `kb_client_memory_find_facts_ex`, and score with the existing `ir_*`/`mem_eval`
  metric shape.
- `locomo`, `longmemeval`, `locomo-qa`, and `longmemeval-qa` return a structured
  `async-only` envelope with `run_via: "aimee memory benchmark <suite>"` instead
  of pulling CLI-only dataset setup or LLM-judge/provider-dependent work into a
  synchronous RPC.
- Unknown suites return a clear error listing known synchronous and async-only
  suites.

The response shape remains stable for synchronous retrieval suites:
`status`, `suite`, `queries`, `labelled`, `errors`, `metrics`, and `latency`.
`optimize run --suite <suite>` already forwards the suite and can now use the
generalized retrieval suites over the RPC surface.

## Verification Notes

The unit coverage in `src/tests/test_server_memory_benchmark.c` exercises:

- live eval-corpus dispatch and scoring for `corpus`;
- preserved `code-graph-fusion` arm/fusion-state resolution;
- `async-only` response behavior for dataset and judge-style suites;
- clear unsupported-suite errors.

User-facing docs and OpenAPI now describe the synchronous suite set and the
async-only boundary for dataset and judge-style suites.
