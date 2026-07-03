# Supervised SWE-bench benchmark

Measures how much of the **expensive primary's** token spend aimee offloads onto a
cheap/free/local delegate fleet, and whether it does so **without a wall-clock
penalty**, at comparable resolution — the same axes as the Reddit result (GPT-5.5
supervising one local worker: −75.5% supervisor tokens but 3.75× slower).

aimee's edge is that the primary supervises **many diverse workers in parallel**, and
that **aimee itself does the coordination and decomposition** (deterministic, no LLM
tokens), so the expensive model only spends tokens on judgment: what to delegate,
reviewing candidates, and selecting the best.

## Arms

| Arm | What runs | Primary tokens |
|---|---|---|
| **A `primary_alone`** | the expensive primary solves each task itself | full |
| **C `supervised`** | N diverse cheap/local workers attempt each task **concurrently**, then the primary reviews the short candidate diffs and selects/synthesizes the best (best-of-N) | tiny — it never reads the code, only candidate diffs |

Worker tokens are free (`token_estimate_cost` prices the fleet at $0) and reported
separately; they are **not** counted against the primary reduction.

## Pipeline

```bash
# 1. Prepare instances (fetch dataset, clone repos, extract the code region).
#    Instance sets:  reddit10 | lite:N (wide sample across all 4 repos) | all (300)
python3 benchmarks/coding/swebench_supervised_prep.py --instances lite:50 \
    --out benchmarks/results/swebench_supervised/regions

# 2. Run the arms against the live aimee (dispatches via the `aimee` CLI).
#    --pool is the cheap/local worker fleet; --primary is the expensive manager.
#    --token-db points at DB1 on the aimee host to read primary tokens.
python3 benchmarks/coding/bench_swebench_supervised.py \
    --regions benchmarks/results/swebench_supervised/regions \
    --arms A,C --primary codex --primary-model gpt-5.5 --n 3 \
    --pool gpu-gemma4,glm-5.2,mimo-2.5,mistral,mimo-2.5-pro,minimax,local-synth \
    --token-db "$AIMEE_HOME/aimee.db" \
    --output benchmarks/results/swebench_supervised/run.json
```

Grading uses the **official SWE-bench Docker harness** (`bench_swebench._grade_with_harness`)
as the sole `resolved` source for both arms; needs Docker + the `swebench` pip package
on the grading host.

The report (primary-token reduction, wall-clock ratio C/A, resolution per arm) prints at
the end of the run and is also derivable from `run.json` via
`supervised_report.render_supervised`.

## Fleet setup

Register the workers first (see `docs/DELEGATES.md`). Concurrency observed: mimo ≥4,
minimax 3–4, mistral 4, glm 4+, codex 2–4. A fast **local GPU worker** matters most for
wall-clock; register one with `aimee agent local <name> <url>/v1 --model <m> --ctx <n>`.

### Local GPU worker (Gemma-4-26B, best-of-N anchor)

On a 16 GB card the winning config is QAT weights + MTP speculative decoding + an 8-bit/4-bit
KV cache so a usable 64k context fits with minimal MoE offload:

```bash
llama-server -m gemma-4-26B-A4B-it-qat-UD-Q4_K_XL.gguf -ngl 999 -fa on \
  -c 65536 -ctk q8_0 -ctv q4_0 \
  --spec-type draft-mtp -md mtp-gemma-4-26B-A4B-it.gguf --spec-draft-n-max 4 --n-cpu-moe 4
# ~128 tok/s, correct, 64k ctx, ~14 GB VRAM.
```

Notes: QAT (14.2 GB) fits 16 GB, standard UD-Q4_K_XL (17 GB) OOMs. MTP needs VRAM headroom
for the draft (garbage below `--n-cpu-moe 4`); the KV quant provides it. `--spec-draft-n-max 4`
is required. On one GPU a single large-context slot beats multiple slots (parallel-slot MTP +
offload contend). Scale to more GPUs for more concurrent large-context workers.

## Reference results (2026-07, reddit10, primary = gpt-5.5 via codex)

| Arm | primary tokens | wall (10 tasks) | resolution |
|---|---|---|---|
| A primary_alone | 104,834 | 434 s serial / 56 s parallel | 8/10 |
| C pure delegation (single attempt, full fleet) | 14,467 (**−86%**) | **36.2 s** | 4–6/10 |
| C best-of-3 + primary selection | 19,700 (**−81%**) | parallel | 5/6 graded |

Headline: the **primary spends 81–86% fewer tokens** (beats Reddit's −75.5%) and the fleet
runs **concurrently** (36 s vs the primary's 434 s serial), i.e. no 3.75× slowdown. Best-of-N +
primary selection recovers resolution toward the solo primary; the current limiter is worker
reliability on long prompts, not the selection mechanism.

## CI / fast check (no live aimee, no Docker)

```bash
AIMEE_BENCH_FAKE_AGENT=1 AIMEE_BENCH_FAKE_GRADER=1 \
  python3 benchmarks/coding/bench_swebench_supervised.py --arms A,C --output /tmp/fake.json
python3 -m unittest benchmarks.tests.test_bench_swebench_supervised
```
