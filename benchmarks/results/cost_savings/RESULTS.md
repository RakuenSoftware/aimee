# Aimee delegation benchmarks — token/cost savings and parallel-decomposition speedup

Primary/frontier model: **gpt-5.6-sol** ("codex"). Cheap worker pool: MiniMax-M3,
mimo-v2.5-pro, kimi-k2.7-code. All numbers measured from the live server `token_audit`
ledger. Frontier-equivalent pricing: **$1.25 / 1M input, $10 / 1M output**.

Two independent levers, measured separately (they do **not** stack — see Caveats):

1. **Token/cost** — delegate the drafting to cheap models; the frontier model only
   supervises. Counts **frontier (manager) tokens only**.
2. **Speed** — split a multi-file task across parallel calls instead of one monolithic
   call. Measured with a **queue-free** latency metric.

---

## 1. Token & cost savings (frontier tokens: codex-solo → aimee-manager)

| Benchmark | Tasks | Frontier tokens | Cost | Token cut | Cost cut |
|---|---:|---|---|---:|---:|
| SWE-bench Lite | 50 | 464,252 → 192,406 | $0.9894 → $0.5368 | −58.6% | −45.8% |
| SWE-bench Lite (reddit10) | 10 | 105,254 → 26,191 | $0.1541 → $0.0498 | −75.1% | −67.7% |
| SWE-bench Verified (multi-file, 3–21 files) | 12 | 335,366 → 43,724 | $0.8229 → $0.2309 | −87.0% | −71.9% |

- Both columns are frontier-model (gpt-5.6-sol) tokens only; cheap-worker tokens are
  **excluded** (verified against the ledger — e.g. the multi-file delegate window contained
  259K of MiniMax/mimo worker tokens that are correctly **not** counted).
- More decomposable → bigger, more consistent savings (multi-file is strongest, every
  instance 72–95%).
- Full per-instance breakdowns: `reddit10.perinstance.json`, `lite50.perinstance.json`,
  `../swebench_multifile/decompose_verified12.json`.

---

## 2. Speed — parallel decomposition (pre-registered, queue-free)

**Question:** on a K-file task, does solving each file in a separate parallel call beat one
model solving all K files at once?

**Result (12 SWE-bench Verified multi-file tasks): parallel faster on 10/12, median 1.82×,
mean 1.95×.**

| Instance | files | monolithic | parallel | speedup |
|---|---:|---:|---:|---:|
| sympy__sympy-13091 | 21 | 115.0s | 22.0s | **5.22×** |
| django__django-15629 | 4 | 81.4s | 32.8s | 2.48× |
| pylint-dev__pylint-4551 | 4 | 94.4s | 38.5s | 2.46× |
| django__django-11532 | 5 | 53.0s | 22.9s | 2.31× |
| sympy__sympy-16597 | 6 | 108.5s | 54.7s | 1.98× |
| astropy__astropy-13398 | 3 | 67.4s | 37.0s | 1.82× |
| django__django-13121 | 4 | 83.0s | 51.1s | 1.63× |
| django__django-11138 | 4 | 81.1s | 50.0s | 1.62× |
| django__django-11734 | 3 | 39.8s | 36.0s | 1.10× |
| django__django-11400 | 3 | 32.7s | 30.6s | 1.07× |
| django__django-16263 | 4 | 80.9s | 82.7s | 0.98× |
| pylint-dev__pylint-6386 | 4 | 27.7s | 39.8s | 0.70× |

- Speedup **scales with file count** (21 files → 5.2×). Parallel latency is capped by the
  slowest single file; the 2 losses are where one file dominates or files are already quick.
- Held **model constant** (codex both arms) to isolate the decomposition+parallelism effect
  from worker speed. Data: `../swebench_multifile/speed_prereg.json`.

---

## Methodology

**Token attribution.** Arms run sequentially as the sole ledger tenant; we snapshot
`MAX(id)` of `token_audit` before/after each arm and sum rows filtered to the frontier
model. A per-run cache-busting nonce forces real spend (no draft-cache freebies). Manager
and worker phases are bracketed separately so worker tokens never leak into the manager
count.

**Latency metric (this is the corrected part).** Latency comes strictly from
`token_audit.duration_ms`, a `CLOCK_MONOTONIC` timer wrapped **only** around the provider
HTTP round-trip (`src/server/agent_runtime.c:1141-1200`) — it **excludes** aimee-side
queue/admission wait. The delegate-job `created_at → updated_at` span was verified to
**include** queue wait (`src/db1/agent_jobs.c:95` inserts the row `pending` at enqueue) and
is **not** used. Because `duration_ms` is queue-free, parallel latency = `max(duration_ms)`
over the K calls is the true "if all K ran at once" wall time, independent of fleet caps.

**Fairness (speed test, pre-registered before running).** Same model both arms; identical
file regions to both; no manager/localize pass on the critical path; one instance at a time;
accept the aggregate whatever the direction.

---

## Caveats (read before quoting)

- **The two wins do not stack.** Token savings require *cheap* workers; the speedup was
  measured with *codex* workers (model held constant). Cheap workers are slower per call, so
  using them for the parallel arm shrinks or erases the latency win. You get cheaper **or**
  faster from a given split, not both maximally at once.
- **Single-file tasks get no speedup.** There is nothing to decompose; delegation there is
  redundant parallelism (best-of-N) which cannot beat one fast model on latency.
- **Some per-task losses are real and left in:** token side has a few negatives (e.g.
  `pytest-11148` +88% tokens); speed side has 2/12 below 1× (`pylint-6386` 0.70×).
- **These runs measure token/cost/latency, not solution correctness.** SWE-bench pass/fail
  grading was not run here.

## Harness / reproduce

- `bench_cost_savings.py` — 3-measurement token/cost (best-of-N supervised).
- `swebench_multifile_prep.py` — prep multi-file SWE-bench Verified instances (region/file).
- `bench_decompose.py` — file-split / subtask-split decomposition arms.
- `bench_speed.py` — pre-registered parallel-vs-monolithic speed test (queue-free duration_ms).
- `bench_reddit_validate.py` — single-file dual-investigation sanity check (tokens + timing).
