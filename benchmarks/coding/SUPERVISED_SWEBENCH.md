# Supervised SWE-bench benchmark (single-shot)

Measures how much of the **expensive primary's** token spend aimee offloads onto a
cheap/free/local delegate fleet, plus wall-clock and official SWE-bench resolution.

## Scope and honesty

This is a **single-shot diff-generation** benchmark: both arms run `--no-tools`, one
prompt → one unified diff. It is **not** the tool-using agentic workflow that the
[Reddit experiment](https://www.reddit.com/r/LocalLLaMA/) measured (GPT-class model
supervising one local worker: reported ~−75.5% supervisor tokens but ~3.75× slower), so
it is **not a like-for-like reproduction** of that result. Treat the Reddit numbers as
**motivation**, not a head-to-head baseline.

What this benchmark *does* show, rigorously: given the same code region, delegating the
diff to a cheap fleet and having the primary only **select the best of N** costs the
primary far fewer of its own tokens than solving each task itself, and the fleet runs
concurrently. What it does **not** show is that aimee's coordination beats an agentic
supervisor — that requires the agentic version below.

> **Follow-up (the real Reddit-parity claim):** a tool-using agentic version — workers
> explore/edit/test the repo, the primary supervises across turns — is tracked
> separately. Only that version should be posted as an apples-to-apples Reddit comparison.

## Arms

| Arm | What runs | Primary tokens |
|---|---|---|
| **A `primary_alone`** | the primary produces the diff itself | full |
| **C `supervised`** | N **distinct** cheap/local workers attempt each task **concurrently**, then the primary reviews the candidate diffs and selects/synthesizes the best (best-of-N) | the primary reads candidate diffs, not the code region |

Worker tokens are free (`token_estimate_cost` prices the fleet at $0) and reported
separately; they are **not** counted against the primary reduction.

## Measurement rigor (what the roundtable required)

- **Primary tokens are scoped to the exact primary job ids this run dispatched.** A
  delegate turn's `token_audit.delegation_id` is `deleg-<n>-<ts>-<job_id>`; the meter sums
  only rows ending in `-<job_id>` for the run's primary jobs. No time-window or
  cross-agent/cross-session contamination.
- **Wall-clock brackets the whole arm** (dispatch → grade); dispatch and polling are
  thread-parallel, so the number is fleet concurrency, not driver serialism.
- **Two resolution denominators.** `resolved/submitted` (skill, given a patch was
  produced) and `resolved/instances` (end-to-end, incl. worker failures — the honest
  public number). Worker failures cannot silently flatter the reduction.
- **Candidate-set health.** Instances with `< ceil(N/2)` candidates are flagged; a flaky
  fleet returning fewer candidates shrinks the selection prompt and would otherwise inflate
  the reduction.
- Only `done` delegate jobs count; `failed`/`error`/`partial` are discarded. Trailing prose
  is stripped from diffs so it is never submitted as a patch.

## Pipeline

```bash
# 1. Prepare instances (fetch dataset, blobless-clone repos, extract the code region).
#    Instance sets:  reddit10 | lite:N (wide sample across all 4 repos) | all
python3 benchmarks/coding/swebench_supervised_prep.py --instances lite:50 \
    --out benchmarks/results/swebench_supervised/regions

# 2. Run the arms against the live aimee (dispatches via the `aimee` CLI).
python3 benchmarks/coding/bench_swebench_supervised.py \
    --regions benchmarks/results/swebench_supervised/regions \
    --arms A,C --primary codex --primary-model <ledger-model-name> --n 3 \
    --pool glm-5.2,mimo-2.5,mistral,mimo-2.5-pro,minimax,local-synth \
    --token-db "$AIMEE_HOME/aimee.db" \
    --output benchmarks/results/swebench_supervised/run.json
```

`--primary` must **not** appear in `--pool`. `--primary-model` is the `token_audit` model
name of the primary (required to read its tokens). Grading uses the **official SWE-bench
Docker harness** (needs Docker + the `swebench` pip package on the grading host).

## Fleet setup

Register the workers first (`docs/DELEGATES.md`). A fast **local GPU worker** matters most
for wall-clock. Reference config for a Gemma-4-26B worker on a 16 GB card (QAT weights +
MTP + 8-bit/4-bit KV so a 64k context fits with minimal offload):

```bash
llama-server -m gemma-4-26B-A4B-it-qat-UD-Q4_K_XL.gguf -ngl 999 -fa on \
  -c 65536 -ctk q8_0 -ctv q4_0 \
  --spec-type draft-mtp -md mtp-gemma-4-26B-A4B-it.gguf --spec-draft-n-max 4 --n-cpu-moe 4
```

Notes: QAT (14.2 GB) fits 16 GB, standard UD-Q4_K_XL (17 GB) OOMs. MTP needs VRAM headroom
for the draft (garbage below `--n-cpu-moe 4`); the KV quant provides it. `--spec-draft-n-max 4`
is required. Scale to more GPUs for more concurrent large-context workers.

## Reference results

Run the pipeline against your own fleet and record `run.json` here with the model names and
run date filled in — do **not** post hand-copied numbers. On the reddit10 set with a strong
primary this repo has observed primary-token reductions in the **~80%** range with the fleet
running concurrently and best-of-N recovering resolution toward the solo primary; fill this
table from a real `run.json`:

| Arm | primary tokens | wall (s) | resolved/submitted | resolved/instances |
|---|---|---|---|---|
| A primary_alone | _fill from run.json_ | | | |
| C supervised (best-of-N) | _fill from run.json_ | | | |

## CI / fast check (no live aimee, no Docker)

```bash
AIMEE_BENCH_FAKE_AGENT=1 AIMEE_BENCH_FAKE_GRADER=1 \
  python3 benchmarks/coding/bench_swebench_supervised.py --arms A,C --output /tmp/fake.json
python3 -m unittest benchmarks.tests.test_bench_swebench_supervised
```
