# Agent-facing code-intelligence paired evaluation

Status: pending

## Goal

Run the provider-backed paired coding matrix needed to decide whether `code_context_mode=on` may
replace `observe` as the default. The product retrieval contract has passed its deterministic E6
corpus, but no eligible fresh standard/observe/on/ceiling agent cells exist at the pinned E6 commit.

## Bounded scope

- use `benchmarks/code-agent-effectiveness/e6-corpus.json` and prompt fixture v1 unchanged;
- execute all eight coding tasks in all four arms from one merged pinned Aimee commit;
- preserve every cell through the E5c checkpoint runner and exclude infrastructure-invalid cells;
- report paired task success, 95% lower confidence bounds, uncached input tokens, total wall,
  actuation-before-edit, retrieval latency, packet tokens, indexing cost, and isolated rebuild cost;
- promote only if every gate in the completed parent proposal remains satisfied.

Until this work merges with a passing result, `code_context_mode=observe` remains the shipping
default. Contract-test success is not authority to promote.
