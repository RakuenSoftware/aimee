# Benchmark provisioning & rollout-validation runbook

This is the operational companion to the (implemented) unified benchmark suite.
The harness, target adapters, pinned-judge plumbing, and per-dataset loaders all
ship in-repo and are unit-tested. What lives **outside** the repo (the real
datasets, the judge model, and the SWE-bench / TerminalBench sandboxes) is
provisioned per host using this runbook.

See the parent (implemented) proposal
`docs/proposals/done/unified-benchmark-suite-memory-coding-reasoning.md` and the
rollout proposal `docs/proposals/accepted/unified-benchmark-suite-rollout-validation.md`.

## 1. Single source of provisioning truth

Every catalog dataset (`benchmarks/catalog.toml`) has a documented provisioning
path in `benchmarks/provisioning.toml`: its source, the file(s) expected under
`data/<bench>/`, whether it can be fetched automatically or needs manual
placement, and whether the source is gated.

The coverage invariant is **enforced**, not aspirational:

```bash
# every catalog dataset must have a provisioning entry (CI gate)
python3 benchmarks/check_provisioning.py --require-coverage

# once data is on disk: show the SHA-256 to pin, and verify already-pinned ones
python3 benchmarks/check_provisioning.py --verify-hashes
```

`check_provisioning.py` reports, per dataset, one of: `bundled`,
`not_provisioned`, `provisioned_unpinned` (prints the SHA-256 to copy into the
catalog `hash` field), `provisioned_verified`, or `hash_mismatch`.

## 2. Fetching datasets

```bash
# memory fast path (locomo + longmemeval_s; add _m/_l with the env var)
scripts/download-memory-benchmarks.sh
AIMEE_FETCH_LONGMEM_ML=1 scripts/download-memory-benchmarks.sh

# everything else, manifest-driven (auto-fetches what it can, prints manual steps)
scripts/provision-benchmarks.sh                  # all pillars
scripts/provision-benchmarks.sh --pillar coding  # one pillar
scripts/provision-benchmarks.sh --dataset gsm8k  # one dataset
scripts/provision-benchmarks.sh --dry-run        # plan only
```

`auto` datasets fetch directly. `auto_hf` datasets fetch from a Hugging Face
`resolve` URL (set `HF_TOKEN` for gated ones); HF *landing pages* that need an
export step are reported as manual. `manual` datasets print the exact placement
instructions (clone / generate / gated download). After every placement the
script prints the SHA-256 to pin in `catalog.toml`.

### Pinning a hash

1. Place / fetch the dataset.
2. `python3 benchmarks/check_provisioning.py` → copy the printed `sha256=` into
   the matching `[dataset.<id>].hash` in `benchmarks/catalog.toml`.
3. Re-run with `--verify-hashes`; the dataset now shows `provisioned_verified`.

## 3. Judge model

The LLM-judge memory track (`locomo`, `longmemeval_*`, `msc`, `dmr`, `beam`) and
any `grader = "llm"` dataset route through the configured judge:

- `catalog.toml [judge] profile = "frontier"` uses the operator's configured
  execute-role agent (`~/.config/aimee/agents.json`).
- `profile = "open70b"` uses a pinned open-weights GGUF; record its digest in
  `[judge].hash`.
- `profile = "small"` is CI-smoke only; results are **not** canonical.

`majority = 3` → three judge votes per question; `check_determinism.py` asserts
the verdicts are stable across reruns at temperature 0.

### Delegate as judge (no GPU)

The `frontier` profile routes both answering and judging through
`aimee delegate execute`, i.e. the configured execute-role **delegate**
(MiniMax / MiMo / Mistral). No GPU or pinned local weights are required to score
the LLM-judge track; the delegate *is* the judge. The harness queues each turn
as a background delegate job and polls `jobs status` (foreground delegate is
unsupported over the default `/v1` transport), so a running `aimee-server` with
the execute agent configured is the only prerequisite.

Knobs (all optional):

- `AIMEE_BENCH_PERSONA`: delegate persona (default `engineer`; required by the server).
- `AIMEE_BENCH_JUDGE_MAX_TOKENS`: judge token budget (default `64`). Raise to
  ~2048 for *reasoning* delegates, which spend tokens on hidden reasoning before
  emitting the JSON verdict and otherwise return "no content".
- `AIMEE_BENCH_DELEGATE_TIMEOUT` / `AIMEE_BENCH_DELEGATE_POLL`: per-job timeout
  and poll interval (seconds).

The adapter LLM targets (`model_only`, `small_agent`, `rag_chromadb`) score the
LLM-judge track with **no** memory store/search, so no vector DB is needed:

```bash
benchmarks/suite/run-llm.sh --target model_only --bench longmemeval_s
```

## 4. Sandboxes (coding pillar)

- **SWE-bench** (`swebench_lite`, `swebench_verified`): `pip install swebench`
  and a working Docker daemon. The official harness pulls a base image plus a
  per-instance child image; pin the base image digest and record it alongside
  the dataset hash. Grading reports a resolved rate (patch → harness → resolved).
- **TerminalBench**: install the `terminal-bench` package and place its task set
  at `data/terminalbench/tasks`; it needs an agentic terminal sandbox.
- **aider_polyglot**: clone the polyglot-benchmark repo into
  `data/aider_polyglot/`.

## 5. Running the validation (one command)

```bash
# full live run on a provisioned host (GPU + judge + Docker)
benchmarks/suite/run-validation.sh \
    --benches locomo,longmemeval_s \
    --targets model_only,aimee \
    --baseline rag_chromadb

# exercise the whole runbook with no external models (what CI smoke runs)
AIMEE_BENCH_FAKE_AGENT=1 AIMEE_BENCH_MAX_SAMPLES=2 \
  benchmarks/suite/run-validation.sh --benches locomo --skip-determinism
```

The runbook runs five steps: provisioning preflight → end-to-end tracks per
target → determinism (same `target,bench,seed` twice) → cross-target comparison
report under `benchmarks/results/comparison_<bench>_v<sha>.{json,md}` → smoke
budget (<15 min).

It is **capability-aware**: it runs every track the host *can* run and cleanly
SKIPS the rest with a reason, then prints a ran/skipped/failed summary and only
fails on a track that *should* have run. So the same command works on a laptop
with no Docker, a GPU box, or CI.

- Coding benches skip where Docker is absent.
- Memory benches skip where the dataset isn't provisioned.
- The `aimee` target writes to a vector DB, so it is **skipped unless**
  `AIMEE_BENCH_ALLOW_AIMEE=1` (point `AIMEE_BENCH_SOURCE_HOME` at a scratch home
  on an isolated DB first); this keeps benchmark runs out of operational memory.
- Default non-aimee baseline is `bm25` (dependency-free); `rag_chromadb` needs
  the `chromadb` package.
- `--max-samples` / `--max-questions` bound the work (LoCoMo carries ~100+
  questions per conversation, so cap them for a quick run).

## 6. Reference numbers & determinism

- **Reproduction:** run `locomo` and `longmemeval_s` against `aimee` with the
  pinned judge; compare overall accuracy to the published anchor (see the BM25
  parity table in `docs/BENCHMARKS.md`). Document any >1pp variance in
  `benchmarks/locomo/BENCHMARK_RESULTS.md`.
- **Determinism:** `run-validation.sh` reruns the first bench twice and runs
  `check_determinism.py --strict`; identical verdicts are required.

## 7. Host notes

The GPU host (pve) carries the corpora and the GPU for real-model runs; the
SWE-bench Docker harness runs on the Docker host. These are shared-infra,
operator-driven runs; they are not executed from CI. CI runs only the
deterministic gates (provisioning coverage) and the fake-agent smoke slice.
