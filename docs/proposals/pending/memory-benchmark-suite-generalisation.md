# Proposal: generalise the `memory.benchmark` RPC beyond code-graph-fusion

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter role(s):** evaluation / benchmark surface (kb-backed retrieval eval;
  no new store, no new DB tier — reuses the existing `mem_eval_*` scoring and the
  delegate LLM-judge track).
- **Scope:** `src/server/server_memory_benchmark.c` (the `memory.benchmark`
  handler — dispatch on `suite`), `src/server/kb_client.c` (reuse
  `kb_client_memory_load_eval_corpus`), the `mem_eval_*` scoring helpers
  (shared), the delegate-judge path (`benchmarks/judge-calibration`, delegate
  jobs) for QA-style suites, `src/cmd_optimize.c` + `src/cmd_memory_embed.c`
  (clients already pass `--suite` through), docs. No new long-lived service.

## Summary

`memory.benchmark` (the only benchmark reachable over /v1) runs **exactly one**
suite — `code-graph-fusion` — and hard-rejects everything else
(`server_memory_benchmark.c:67`). The other eval suites (`memory`/`corpus`,
`locomo`, `longmemeval`) live **CLI-side** in `cmd_memory_embed.c` and are
unreachable from the thin client, so `aimee optimize run --suite memory` (and the
benchmark RPC generally) errors. This proposal generalises the RPC to dispatch on
`suite`, porting the suites server-side in value order, and is explicit about the
one that genuinely cannot be a synchronous RPC.

## Motivation (and an honest priority note)

This is **deliberately lower priority** than the rest of the optimization-surface
work, and the proposal should say so: the suite the optimize **decision points**
actually exercise — `code-graph-fusion`, for the retrieval points
`kb_memory_retrieval_limit` and `kb_fusion_mode` — already works over RPC. The
other suites are *general* retrieval/QA benchmarks, orthogonal to closing the
bandit loop. So the value here is "expose the full benchmark battery over /v1 (and
to `aimee optimize run`)", not "unblock the optimizer." The optimization-surface
P2 explicitly sanctioned deferring this; this proposal is that deferred work,
scoped honestly.

## Current state (verified)

- `handle_memory_benchmark` is **self-contained for code-graph-fusion**: it loads
  a committed corpus via `mem_eval_load_production_corpus`, runs recall against
  the live store for one ablation arm, and scores MRR / nDCG@{5,10} / recall@{5,10}
  + latency. No LLM, no external corpus.
- The other suites are CLI-only:
  - `memory` / `corpus`, `locomo` — **retrieval-style**: load an eval corpus
    (`kb_client_memory_load_eval_corpus`, which is **server-reachable**) and score
    with the same `mem_eval_*` metrics. Portable server-side.
  - `longmemeval` (and other QA suites) — **judge-style**: answers are graded by
    an **LLM judge** (the delegate-based judge track). Inherently asynchronous and
    GPU/provider-dependent — not a synchronous RPC.

## Design

### `memory.benchmark` dispatches on `suite`

Replace the single-suite guard with a dispatch:

- `suite == "code-graph-fusion"` → today's path (unchanged).
- `suite ∈ {"memory","corpus","locomo"}` → a shared **retrieval-eval** path: load
  the suite's corpus (`kb_client_memory_load_eval_corpus` with the suite's basis),
  run recall per case, score with the existing `mem_eval_*` helpers, return the
  same scores shape. This is a server-side mirror of the CLI corpus eval, reusing
  the metric code — not a new scorer.
- `suite ∈ judge-style (e.g. "longmemeval")` → **not a sync RPC.** Return a
  structured `{ status: "async-only", run_via: "aimee memory benchmark longmemeval" }`
  rather than pretending; optionally enqueue a delegate-judge job and return a job
  handle the caller polls (P3).

Unknown suites return a clear `unsupported benchmark suite (known: …)` error that
lists the supported set, instead of the current `only code-graph-fusion`.

### Clients

`aimee optimize run --suite <X>` and `aimee memory benchmark <X>` already pass the
suite through; they need no change for the retrieval suites, and for judge-style
suites they surface the `async-only` result with the pointer to the CLI path.

### What this deliberately does **not** do

- It does **not** move the LLM-judge into a synchronous RPC — judge-style suites
  stay async (run via the CLI/delegate track), with at most a polled job handle.
- No new benchmark scorers — reuse `mem_eval_*`.
- No new corpora or assets shipped; suites resolve their committed corpus paths
  as code-graph-fusion already does.

## Phasing

- **P1** — dispatch on `suite` + the shared retrieval-eval path for the `memory`
  / `corpus` suite (server already has `kb_client_memory_load_eval_corpus` + the
  scorer). Clear "unsupported suite (known: …)" error for the rest.
- **P2** — add `locomo` (another retrieval suite) to the shared path.
- **P3** — judge-style suites (`longmemeval`): enqueue a delegate-judge job and
  return a pollable handle (or keep them CLI-only with the `async-only` pointer if
  the polling surface isn't wanted).
- **P4** — once ≥2 suites work, let `aimee optimize run --suite <X>` rank arms on
  any retrieval suite (the optimize CLI already forwards `--suite`).

## Risks

- **Server/CLI scorer drift.** The retrieval path must reuse the exact
  `mem_eval_*` helpers the CLI uses, or server and CLI numbers diverge. Factor the
  shared scorer; do not re-implement.
- **Corpus availability at runtime.** The eval corpora must be resolvable from the
  server's working dir (as the committed code-graph-fusion assets are); document
  the path contract.
- **Scope creep into the judge track.** Judge-style suites are a different beast
  (async, GPU, provider); keep them explicitly out of the sync path (P3 only,
  behind a job handle) so this doesn't balloon.
- **Low ROI.** This does not serve the bandit decision points (code-graph-fusion
  already does); prioritise accordingly.
