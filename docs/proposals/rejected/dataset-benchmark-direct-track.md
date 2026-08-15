# Proposal: restore a dataset benchmark track (LoCoMo / LongMemEval)

- **State:** REJECTED — archived 2026-08-14 under the Go-or-rejected policy.

## Decision

This proposal is rejected under the current implementation policy: pending proposal work must be
implemented in Go or moved to `rejected/`. The recorded restoration path is a C benchmark-module
entry point in `aimee-kb`, backed by C DB2/pgvector code and the rejected C/SQL scratch-store
design. The synchronous `memory.benchmark` service explicitly returns an async-only envelope for
LoCoMo and LongMemEval rather than hosting those suites.

Porting the benchmark engine and its scratch-store ownership to Go would be a new architecture, not
completion of this direct-track proposal. It would also duplicate the existing benchmark and DB2
owners. This rejection does not remove working coverage: the per-PR LLM regression track, live
retrieval suites, and poison gate remain unaffected as recorded below.

## Archived technical record

The direct track was deleted rather than repaired; this records what it did, why it went, and what
restoring it costs. It was not urgent because the per-PR regression gate that exercises these
datasets remained unaffected.

## Why this exists

`benchmarks/common/native_direct.py`, `benchmarks/run-direct.sh` and the two
`bench_aimee_direct.py` entry points were removed. They invoked
`<root>/aimee-client eval <suite> --dataset ...`, and `aimee-client` is in
`RETIRED_ROOT_BINARIES` — nothing builds it, and both `retire-obsolete-binaries`
and `clean` delete it. The track had been **unrunnable, not merely unrun**, since
that retirement.

This is the reason `benchmarks/memory/BENCHMARK_RESULTS.md` records the quality
and latency rollout gates as "Not yet measured": nobody could have measured them.
It was not a missing bench host or missing datasets.

Deleting was the right immediate call — a harness that cannot run is worse than no
harness, because it reads as coverage. But the *capability* it represented is
worth having, and this proposal is the record of that.

## What is still covered, so nobody re-derives it

Losing the direct track did **not** lose LoCoMo/LongMemEval coverage:

- `benchmarks/smoke.sh` → `benchmarks/run-llm.sh` → `bench_aimee_llm.py`, run on
  every pull request by `.github/workflows/bench-smoke.yml`, comparing PR branch
  against base with `locomo-mini` / `longmemeval-mini` fixtures and
  `AIMEE_BENCH_FAKE_AGENT=1`. The LLM track never touched `native_direct.py` and
  is untouched by the deletion.
- `memory.benchmark` through `aimee-server` covers the live-retrieval suites
  (`code-graph-fusion`, `memory`, `corpus`, `memory-retrieval`, `live`).
- `benchmarks/memory/poison_gate.py` runs standalone; gate passes.

What was lost is specifically the **dataset-driven, non-LLM retrieval** measurement
— NDCG/Recall/MRR over a labelled corpus with a miss breakdown, without a judge
model in the loop. That is the shape that would answer "did this retrieval change
help", cheaply and deterministically.

## What restoring it needs

1. **A binary that hosts the suites.** The implementation is in the benchmarks
   module and needs DB2 and pgvector directly (`mem_eval_open_temp_db` opens a
   scratch store, sets the embedding dimension, drains async queues).
   `aimee-server` links no DB2 — `$(SERVER)` takes `$(DB1_OBJS)` only — so it
   cannot host them; that was tried and fails at link. `aimee-kb` can, and a
   `--eval` entry point there was prototyped and dropped with the track.
2. **A working scratch store.** Blocked today on the shadow-schema conflict described in the
   [rejected C/SQL remediation record](../rejected/eval-temp-store-schema-relocation.md). Under the
   Go-or-rejected policy, any revival needs a new Go-owned design that avoids the C scratch store;
   otherwise the suites still have nowhere to load a corpus.
3. **Embedder wiring.** `config_embedder_command` resolves `EMBEDDER_URL`, then
   the `embedder_command` field — never `embedder_model`. The kb entrypoint
   exports `EMBEDDER_URL` only into processes it spawns, so any separately-exec'd
   harness must set it explicitly.
4. **Datasets.** `scripts/download-memory-benchmarks.sh` fetches them; they are
   not in-tree.
5. **A reporting decision.** The old harness parsed a fixed stdout format
   (`MRR:` / `NDCG@10:` / `Latency: p50=..p99=..`). If the entry point is
   rebuilt, emitting JSON directly is strictly better than reproducing a text
   format a regex has to re-parse.

## Worth doing at the same time

Whatever replaces this should report the triple **accuracy / latency /
injected-tokens together**, not accuracy alone. Aimee already collects all three
and never shows them side by side, so a change that buys two points of accuracy at
three times the injected tokens currently looks like a win. That reporting change
is independent of the harness and cheap.

## Sequencing

Replace the [rejected C/SQL scratch-store design](../rejected/eval-temp-store-schema-relocation.md)
first — without a working storage path there is nothing to build on. Then (1) and (3), which are
small. (4) and (5) are routine.

Do not start this to chase the two open rollout gates in
`BENCHMARK_RESULTS.md` unless someone wants those specific numbers; the per-PR
bench-smoke gate is what actually protects against regressions today.
