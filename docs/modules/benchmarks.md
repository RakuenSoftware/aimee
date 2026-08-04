# benchmarks module

## Purpose and non-goals

`benchmarks` is an optional quality-measurement module that owns offline benchmark harnesses, suites,
corpora, scoring, baselines, result aggregation, and benchmark cadence. It is not named `evals` and does
not own runtime agent assessment, production verification, routing/policy decisions, roundtable
verification, workflow approval, memory behavior, or general telemetry.

### Go process stages

The supervised benchmarks process uses the shared pure-Go module runtime for two
bounded deterministic stages. Its BIRQ/BIRS scoring contract accepts up to 32
retrieved and relevant identifiers plus K, then returns the existing MRR, NDCG@K,
and recall@K definitions. Its BLRQ/BLRS latency contract accepts up to 512
non-negative finite measurements, then returns nearest-rank p50/p95/p99 plus the
minimum and maximum. The C adapter is a wire-parity fixture. Dataset loading,
benchmark execution, providers, scratch databases, raw timing capture, baselines,
reporting, and result persistence remain in their current C and script owners
while those boundaries are migrated. The production server's live
`memory.benchmark` RPC sends every bounded result set and latency set through
these event-bus stages and fails the run when the process is unavailable or
returns invalid wire data; it has no local scoring or percentile fallback.
Offline harnesses continue to use the module's C scoring primitives without
requiring a daemon bus.

## Public contracts

The module directory `src/modules/benchmarks/` owns four production C sources: `agent_eval.c` (shared eval
machinery: case scoring, latency buckets, temp-db bootstrap, progress files), `agent_eval_baseline.c`
(regression baseline load/compare/save), `agent_eval_benchmarks.c` (LoCoMo and LongMemEval dataset
runners), and `agent_eval_memory_support.c` (memory-retrieval eval support), plus the process
wire-parity fixture `module_adapter.c`. Two module-root
headers: `agent_eval.h`, the public contract consumed by CLI, server, and test callers through
`-Imodules/benchmarks`, and `agent_eval_internal.h`, the private seam shared across the four sources.
This code was relocated from the former non-descriptor `src/modules/agent_eval/` directory, whose
name the canonical taxonomy forbids; the `agent_eval_` symbol prefix is retained as the framework's
API identity. `memory.benchmark` and `eval.run` route handlers, benchmark scripts/catalogs, and CI
smoke gates remain owned elsewhere and consume this module rather than belonging to it.

## Dependencies and consumers

- `config`: supplies benchmark provider, corpus, arm, threshold, and execution settings.
- `ir`: supplies canonical inputs/results suitable for comparable scoring and attribution.
- `memory`: exposes retrieval behavior and benchmark-only scratch seams without transferring ownership.
- `module-runtime`: supplies optional lifecycle, capability, and readiness contracts.
- `routing`: selects benchmarked providers/arms without allowing benchmark code to change live routing.

Consumers include developers, CI, `aimee agent eval`, `aimee memory benchmark`, `eval.run`, optimize
comparison gates, dashboards, and research scripts. Runtime KB ranker promotion gates consume benchmark
results but remain owned by learning/memory, not this optional harness module.

## Providers and readiness

Providers include C agent/memory harnesses, Python coding/reasoning/memory/ingest suites, external
datasets such as `LoCoMo` and LongMemEval, scratch DB providers, CI smoke jobs, and stored baselines.
Readiness must report harness presence, dataset/license/download state, provider credentials, scratch
isolation, deterministic seed/arm, metric support, and result destination separately.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the descriptor is `enabled_by_default: false`, so the target module is selected before startup rather than hot-toggled.

No `modules.benchmarks` activation field or target-module registration was found. The legacy
`src/modules/agent_eval` objects are included in `AGENT_SRCS`, and server/CLI benchmark routes remain
available in ordinary builds. Descriptor-declared optionality is therefore `not present` at the current
physical/build boundary; this slice documents the mismatch without changing it.

## Surfaces

Surfaces include `aimee agent eval`, `eval.run`, `aimee memory benchmark`, `memory.benchmark`, optimize
run/compare, benchmark scripts, dataset provision/download scripts, baseline/result files, dashboard
summaries, and `bench-smoke.yml`. Names may retain legacy `eval` compatibility during migration, but the
module taxonomy and new ownership language use `benchmarks`.

## Data and migrations

Data includes task suites, corpora, expected answers/relevance, arm matrices, model/provider metadata,
latency/cost/quality metrics, miss traces, progress, baselines, result JSON/`JSONL`/text, and temporary
isolated DBs. Results require provenance for code/data/config/model versions. Moving legacy agent-eval
storage or APIs requires explicit aliases and must not rewrite historical benchmark evidence.

## Security and privacy

Datasets, repositories, prompts, model output, credentials, result artifacts, and third-party licenses
are untrusted or sensitive. `benchmarks` must use scratch stores and bounded subprocess/network access,
redact secrets and private examples, prevent result contamination, and never mutate production memory,
routing, policy, or provider configuration as a side effect of a measurement run.

## Supported journeys

An operator selects a committed suite/arm and provider; `benchmarks` validates prerequisites, creates an
isolated run, executes comparable cases, records per-case evidence, aggregates metrics, compares an
optional baseline, and emits a provenance-bearing report. CI may fail on a declared regression, but the
module does not make per-request production decisions or participate in roundtable consensus.

## Tests and failure behavior

Benchmark inventory/LLM tests, the extensive `benchmarks/tests` suites, agent/memory evaluation tests,
server memory-benchmark process-parity and fail-closed tests, corpus validators, and smoke workflows
cover current harnesses. Missing dataset/provider, scoring or latency process stage, invalid case,
scratch-store failure, timeout, incomplete sample, or baseline mismatch must be explicit;
skipped/incomparable cases cannot be silently scored as passes.

## Operational diagnostics

Report `benchmark` suite/case/arm, corpus and code revision, provider/model, seed, sample/skip/error count,
metrics, latency/cost, baseline delta, scratch isolation, and result path. Exclude credentials, private
dataset rows, raw prompts/responses, and production memory contents. Distinguish harness absence,
provider failure, invalid corpus, incomplete run, and measured regression.

## Compatibility

Suite/task schemas, CLI/API aliases, metric definitions, thresholds, arm names, dataset versions,
baseline/result formats, provenance, and exit semantics are compatibility contracts. Legacy `eval.run`
and `agent_eval_*` names may be transitional aliases, but they cannot preserve a separate `evals` module
or imply runtime evaluation authority.

## Extension and removal

New suites register offline harness/data/metric contracts without adding runtime decision hooks.
`src/modules/agent_eval`, `server_eval.c`, benchmark routes, and distributed scripts are `relocate` or
compatibility-alias candidates for a later source slice. Stale corpora, duplicate runners, committed
sample results, and self-tested-only harnesses are candidates, not confirmed dead; removal requires
consumer, CI, reproducibility, licensing, and liveness evidence.
