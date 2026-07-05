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

---

# Agentic variant (issue #987) — the true, tool-using, Reddit-parity claim

The single-shot benchmark above delegates one diff and reviews it; it is **explicitly barred**
from being posted as a Reddit head-to-head. The *agentic* variant makes both arms tool-using
across turns and is the only one permitted to carry the public claim. Design + rulings:
`docs/proposals/pending/agentic-supervised-swebench.md`. Built slice-by-slice, each
roundtable-reviewed; every reproducibility-critical function is pure and unit-tested, with the
live parts (server transport, per-worker container, official grading) as marked stubs.

## Module map

| module | slice | role |
|---|---|---|
| `swebench_transport_verify.py` | S0 | 5 token-attribution assertions (primary = `delegation_id` EMPTY; no cross-bill; cache split; primary tools billed) — the blocking transport gate |
| `swebench_agentic_harness.py` | S1 | per-worker workspace provision at `base_commit`, per-worker OS-resource allocator, canonical secret-redacted patch (`git add -A` → `diff --cached`), loop bounds |
| `swebench_arm_runner.py` | S2 | arm A runner + shared measurement core: **uncached** primary-token headline, two wall-clocks (total vs work) |
| `swebench_supervision.py` | S3 | arm C honesty core: hard-capped leak-guarded digests, **deterministic** best-of-N, gated escalation, context fold, tool allowlist |
| `supervised_report_panels.py` | S4 | BCa CI, Pareto, selection-skill (oracle vs actual), escalation exclusion, context drift, two-wall-clock, arm-parity |
| `swebench_suite.py` | S5 | run-plan matrix, CT-101 grader lease, grader-retry + flip-detection, prediction dedup |
| `swebench_claim_gate.py` | S6 | the fail-closed public-claim gate |

## The fail-closed claim gate

The public line ("beats Reddit's −75.5% at no wall-clock penalty") is emitted by
`swebench_claim_gate.evaluate_claim_gate(b1, b2, independent_review=…)` **only if ALL** hold,
under the official grader, on **both** Benchmark 1 (Reddit-10) **and** Benchmark 2 (held-out):

1. **Token** — BCa-95 CI lower-bound of the primary-token reduction `> 0`, anchored on **N=1**.
2. **Wall** — **p95** A→C wall-clock ratio CI upper-bound `≤ 1.0` (vs aimee's own arm-A time,
   not Reddit's cross-harness minutes).
3. **Resolution floor** — `resolved_C/total ≥ max(0.7 × resolved_A/total, 0.25)`.
4. **K ≥ 10** repeats on Benchmark 1.
5. **Not escalation-dominated** — the arm-C headline set's escalation-excluded fraction `≤ 0.40`.
6. **Independent reviewer** sign-off (not the harness/worker author).

The report is **published regardless** of the outcome; only the claim *line* is gated. When
withheld, the summary still reports the honest number, e.g.
`-78% primary tokens at 1.05x p95 wall-clock — CLAIM WITHHELD (B1[wall] …)`.

## CI fast check (no live aimee, no Docker)

```bash
python3 -m unittest \
  benchmarks.tests.test_swebench_transport_verify \
  benchmarks.tests.test_swebench_agentic_harness \
  benchmarks.tests.test_swebench_arm_runner \
  benchmarks.tests.test_swebench_supervision \
  benchmarks.tests.test_supervised_report_panels \
  benchmarks.tests.test_swebench_suite \
  benchmarks.tests.test_swebench_claim_gate
```

## Live run (needs the .254 fleet + CT 101 real docker)

The live transport, per-worker container, and official grading are marked
`NotImplementedError` stubs that print the exact wiring step. Wire them against the `.254`
fleet (`run_agentic_loop`, `run_arm_a`, `run_arm_c_supervised`, `run_suite`) and grade on CT 101
per the recipe above; feed the records through `supervised_report` + `supervised_report_panels`,
then `swebench_claim_gate`.
