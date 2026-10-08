# Context paging: remaining producers, measurements and integrations

- **State:** pending.
- **Consolidated — 2026-10-08:** residual work from this proposal, `context-paging-s2-s5.md` and `compaction-quality-baseline.md`.

## Completed slices

Record-based compaction (S1), persistent fold page tables (S2c), exact-coordinate recall (S2d) and recall precision (S5a) are implemented. See [the archived implementation and dated amendments](../done/context-paging-s2-s5.md). Continuous fold machinery also exists and remains default-off; the proposal must not call it an unbuilt mechanism. Current ownership spans the C agent host and Go economizer; old C source names/line numbers are historical.

## Pending scope, in dependency order

1. S2a task-rail producer: declare a real plan at the agent seam, measure actual updates, then persist. Zero production callers cannot be repaired by persisting an empty recorder.
2. S2b episode-seal producer: derive file inventory from exact closet PATH coordinates and conclusions from register-tagged verdicts at the fold boundary; measure production before persistence.
3. S3 extend `benchmarks/compaction-quality` to compare fold views and compactor summaries on exact-coordinate retention, recall precision and realized cache-read/cache-write tokens across real turn sequences. Measure before enabling the fold. Reuse the reducer's state and freeze; the old stateless `context_engine` ABI cannot preserve a per-conversation boundary.
4. S5b publish a reproducible rounds-to-resume baseline: frozen transcripts with paths, errors, decisions and constraints; random eviction/topic return/restarts; paired prompt/model/budget identities; failure counts, latency and token cost. Track useful recovered information rather than plausible summaries.
5. S0 investigate client-side compaction and a usage-cause signal only after evidence shows it is required. S4 Anthropic/session-key gateways and conditional continuous-fold integration remain separately gated by real client behavior and measurements.

## Acceptance

Eviction conserves exact coordinates or reports explicit inability; recall reconstructs the requested span, not a paraphrase. Producer events are durable and correctly scoped. A powered paired comparison shows retention/precision noninferiority and acceptable real prompt-cache behavior before rollout, with rollback. No claims that users cannot notice a boundary or that client compactors are replaced until measured on the actual clients.
