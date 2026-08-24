# Agent-facing code-intelligence paired evaluation

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Status: done — the complete provider-backed matrix passed and promoted `on`

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

The fresh matrix at pinned merge `aa8c40e9d75449774c9b0b630bb8f1037efb8097` completed all 32
cells with no infrastructure exclusions. `on` passed the success-confidence, wall-efficiency, and
actuation gates, so this proposal promotes `code_context_mode=on`. The validation report records
the exact checkpoint, artifacts, denominators, and review trail.
