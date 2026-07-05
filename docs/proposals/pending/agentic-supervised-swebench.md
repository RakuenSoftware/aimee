# Proposal: Agentic supervised SWE-bench — a true, tool-using, Reddit-parity claim

- **State:** PENDING — net-new benchmark harness on top of shipped substrate.
  Directly resolves [issue #987](https://github.com/RakuenSoftware/aimee/issues/987)
  and is the *only* variant permitted to carry a public
  "beats Reddit's −75.5% supervisor-token reduction **at no wall-clock penalty**"
  claim. Builds on **PR #986** (single-shot supervised SWE-bench: `token_audit`
  job-id scoping, concurrent Fleet dispatch/poll, `swebench_supervised_prep.py`
  instance-set prep, official-grader recipe) and reuses three already-merged
  substrates rather than re-proposing them: the **autonomous-dev execution
  substrate** (`git_verify format=json` mechanical verify gate, bounded
  implement→verify loop — `done/autonomous-dev-execution-substrate.md`), the
  **per-work-item git worktree isolation** (`aimee/wi/<id>`, flock-serialized —
  `done/full-autonomous-development.md`, F2), and the **fleet delegate/gateway**
  path (`gateway_delegate.c`, `delegate_launch.c`, encrypted vault, health probe +
  circuit-breaker). It does **not** re-propose any of those; it composes them into
  a graded, apples-to-apples *agentic* comparison.
- **Author:** JBailes
- **Date:** 2026-07-05
- **Charter roles:** Reason (supervision-across-turns: review, redirect,
  best-of-N selection), Execute (per-worker agentic explore/edit/test loop),
  Persist (durable `token_audit` job-id-scoped ledger + per-instance run records),
  Calibrate (aggregate `sum(A)/sum(C)`, bootstrap CIs, sensitivity over N),
  Review (official SWE-bench Docker grader is the sole resolution source; a
  public-claim gate that fails closed).

## Thesis

PR #986 proved the *trivial* half of the Reddit claim: if the expensive primary
delegates a one-shot fix and only reviews the returned diff, it obviously spends
fewer of its own tokens (we measured **−81% best-of-3 / −86% pure-delegation**
primary tokens on the Reddit-10). The code roundtable (C6) correctly flagged that
this is **not** a like-for-like reproduction: the Reddit experiment
(`rndhouse/mixmod`) is a **tool-using agentic workflow** — GPT-5.5 supervising a
local Qwen3.6-27B worker that *explores the repo, edits, runs tests, and iterates*
across many turns. A single-shot `--no-tools` diff is a different, easier task.

The Reddit result has two axes and it **won one, lost the other**:

- **Won:** −75.5% supervisor tokens (−76.1% input, −51.4% output).
- **Lost:** wall-clock. Mixmod took **85.7 min** vs **22.8 min** for GPT-5.5 alone
  — **3.75× slower** — because it drove *one* local worker serially.

Aimee's structural advantage is exactly the axis Reddit lost: a **fleet of many
cheap/free/local workers running concurrently**. One supervisor fanning N agentic
workers in parallel should reclaim the wall-clock **while keeping** the
supervisor-token reduction. That is the claim worth making publicly — and it can
only be made honestly from an **agentic** harness where *both* arms are tool-using
across turns. This proposal builds that harness and the full benchmark suite that
substantiates (or refutes) the claim.

## Goal

A graded, reproducible benchmark with two tool-using arms over identical
SWE-bench Lite tasks:

- **Arm A — agentic solo.** The primary drives the full agentic loop itself:
  explore the repo, read files, edit, run tests, iterate to a fix. We count the
  primary's own tokens.
- **Arm C — agentic supervised.** Cheap/local/free **workers** each drive the
  agentic loop (explore/edit/test/iterate) in an isolated repo workspace; the
  **primary supervises across turns** — reviews compact progress summaries and
  candidate diffs, redirects, digs into code *only on failure*, and selects
  best-of-N. We count **only the primary's tokens**.

We report three numbers per benchmark, each with the honesty caveats below:

1. **Primary-token reduction A→C**, job-id scoped (as PR #986), aggregated
   `sum(A)/sum(C)`, with bootstrap CIs.
2. **Wall-clock**, per-instance median/p95 (t0 = prompt enters supervisor,
   t1 = official grader resolves) — the axis Reddit lost. Target: **A→C wall-clock
   ratio ≤ 1.0** (no penalty), ideally < 1.
3. **Resolution parity** via the **official SWE-bench Docker grader**, reported
   against **both** PR #986 denominators (resolved/attempted and resolved/total).

Everything new ships as bench tooling and config; no production default changes.

## Non-goals

- **Not a new agent runtime.** Both arms run on the *existing* agentic substrate
  (autonomous-dev implement→verify loop, worktree isolation, delegate tools). We
  add a benchmark driver + metrics, not a new execution engine.
- **Not a resolution-SOTA attempt.** The headline is the *token × wall-clock*
  trade-off vs the Reddit baseline, not topping the SWE-bench leaderboard. We
  report whatever resolution parity falls out, honestly.
- **Not the single-shot benchmark.** PR #986 stays as the delegation-cost
  micro-benchmark; this is its agentic superset. The single-shot number is
  **explicitly barred** from being posted as a Reddit head-to-head (that
  restriction is the whole reason for #987).
- **Not a fleet-routing change.** We consume today's role→endpoint mapping and the
  health/circuit-breaker signals; failover/cost-routing is owned by
  `resilient-fleet-routing.md`.

## Background — what exists today (do not rebuild)

- **PR #986 (`bench/…`, merged to testing):** `swebench_supervised_prep.py`
  (fetch Lite via HF datasets-server, clone at `base_commit`, extract regions;
  instance sets `reddit10 | lite:N | all-300`), `bench_swebench_supervised.py`
  (arms A/C, **concurrent** dispatch+poll — fixes the sequential-poll wall-clock
  artifact — primary tokens from `token_audit --token-db`),
  `supervised_report.py` (`summarize_arms` / `render_supervised`),
  `SUPERVISED_SWEBENCH.md` runbook. **430 bench tests pass.** This proposal
  *extends* these files; it does not fork them.
- **Token accounting:** `db1/token_audit.{c,h}`. Supervisor tokens = rows with
  `delegation_id` **EMPTY**, `usage_kind='realized'`, for the run's session_id.
  Worker tokens = `delegation_id` **NON-EMPTY**, priced **$0** (free delegates via
  `token_tracker.c` cost model). The `session tokens <sid> --json` CLI split
  (supervisor vs worker) shipped with PR #983.
- **Agentic execution substrate (merged):** the autonomous-dev implement→verify
  loop — `exec_implement` advances a unit only when it passes the
  `git_verify format=json` mechanical gate; bounded by `stage_attempt` / turn +
  wall-clock caps; **per-work-item git worktree** `aimee/wi/<id>` (lazy,
  flock-serialized, partial-state scrubbed). This is the natural per-worker
  agentic harness the issue asks for.
- **Delegate transport:** `delegate execute/draft --via <agent> --persona <p>`
  (async `job_id` → poll `delegate status`) works against the remote **.254**
  fleet. `code` role = **agentic file-editing** — needs a repo checked out in a
  workspace, returns "no file changes detected" / "partial" when there's nothing
  to edit.
- **Fleet reality (.254):** 6–7/7 tier-0 workers healthy
  (mistral / mimo-2.5 / -pro / glm-5.2 / codex[gpt-5.5] / local-synth); **minimax
  "no content" dropout** is a known flaky. GPU-local **`gpu-gemma4`**
  (Gemma-4-26B QAT, MTP, ~128 tok/s, 64k ctx) registered on CT109 / .253 5080 —
  aimee's answer to Reddit's "local Qwen3.6-27B on a 3090".
- **Grading recipe (established):** the official `swebench` Docker harness runs on
  **CT 101 (real docker, python3.11, 64G optane)**. The `.253/.254` LXC2Docker
  **shim mangles** stock images (`python:3.11` has no `/usr/bin/python`) and host
  python 3.13 is too new for 2017 repos → grade on CT 101 only. Harness **dedupes
  by `instance_id`**, so one prediction file per (arm, model, cmp) with a distinct
  `run_id`.

## The measurement problem the agentic arm introduces (and how we solve it)

Single-shot was clean: one primary turn, one worker turn. The agentic arm is
**multi-turn on both sides**, which creates several new measurement hazards
(surfaced/sharpened by the 2026-07-05 roundtable). Each has a concrete control:

1. **Worker turns must never bill the primary.** In arm C the workers run their
   own multi-turn tool loops. Every worker turn must carry a **non-empty
   `delegation_id`** so it is excluded from the supervisor total and priced $0.
   *Control:* the **S0 verification matrix** submits a known agentic loop through
   each candidate transport and asserts the resulting `token_audit` rows land with
   the correct `delegation_id` polarity (empty for the primary's own turns,
   non-empty for worker turns) **before** any graded run. Reusing PR #986's job-id
   scoping is necessary but not sufficient — multi-turn must be re-verified.

2. **The primary must not "cheat" by reading code in arm C.** The entire claim
   depends on the supervisor reading **compact progress summaries and short
   candidate diffs**, never the full source. If the primary opens files on every
   turn, arm C degrades toward arm A. *Control:* a **supervision budget** — the
   primary's per-turn context in arm C is a templated *turn digest* (worker's
   last action, test delta, unified diff of the candidate) with a hard cap;
   full-file reads are permitted **only** on an explicit `escalate` decision after
   a worker fails, and every escalation is logged so the report can attribute how
   much of arm-C's primary spend came from digging in. (This mirrors the PR #986
   finding: savings appear only when the primary avoids reading real code.)

3. **Wall-clock must be per-instance, not fleet-aggregate.** Reddit's 3.75× loss
   is a *per-task latency* number. Running 10 instances concurrently and dividing
   total wall by 10 would be a throughput number that flatters us dishonestly.
   *Control:* pin **`t0` = instance prompt enters the harness dispatch queue —
   identically for both arms** (not "enters supervisor", which is ambiguous for
   arm C and would hide queue time); `t1` = grader resolves. Record and report
   **both** total wall-clock (queue+work) and **work-only** (`t1 − first_worker_start`)
   **per instance**, as **median AND p95** (the gate is on **p95**); fleet
   throughput is reported separately and explicitly labeled. Discard the first
   instance per run as **warmup** (worktree provisioning / model load) and report
   its latency separately. Arm C's win must hold at the **per-instance p95** level.

4. **Primary-side tool use must be mechanically blocked, not conventional.** The
   "no reading code in arm C" discipline is enforced by a **hard tool allowlist**,
   not by prompt convention: the arm-C primary may call only `delegate`,
   `select_best_of_N`, supervisory meta-tools, and a **gated `escalate`** — code-
   reading tools (`read_file`/`bash`/`grep`) are refused unless an escalate token
   is spent, and every escalation is logged and attributed.

5. **Multi-turn token attribution has three hidden leaks — S0 must assert all of
   them.** (a) **Cached vs uncached:** arm A re-reads files across turns and hits
   the prompt cache; cached and uncached primary tokens are split and the
   **uncached** count is the headline denominator for both arms. (b) **Worker
   content billed to primary:** large worker returns (diffs in turn digests) must
   not count as primary input — S0 tests a 5k+-token worker return and asserts it
   is excluded (or carries a `worker_ref` attribution). (c) **Thinking vs text:**
   `primary_thinking` is split from `primary_text` and reported separately, since
   supervisors may reason differently across arms.

6. **Supervisor context drift.** Accumulated turn digests (workers × turns ×
   tokens) grow the primary's per-turn context and erode late-run savings.
   *Control:* cap accumulated supervisor context (sliding window / fold-reducer,
   target ≤16k) and report the `primary_tokens_by_turn` curve so the erosion is
   visible, not hidden in the aggregate.

## Design

### The per-worker agentic harness (arm C worker, and arm A primary)

Both arms need the same loop; they differ only in *who* runs it and *who pays*.

```
provision workspace  → isolated worktree aimee/wi/<instance,arm,worker>
                        at base_commit (reuses F2 worktree isolation)
        │
   ┌────▼─────────────────────────── agentic loop (bounded) ──────────────┐
   │  explore: search/read within the workspace                          │
   │  edit:    apply changes to files                                     │
   │  test:    run the in-loop test command (worker's own signal)         │
   │  verify:  git_verify format=json mechanical gate                     │
   │  iterate until (tests green ∧ verify pass) ∨ turn/wall/USD cap        │
   └──────────────────────────────────────────────────────────────────────┘
        │
   emit: unified diff (patch) = git -C <worktree> diff <base_commit>
```

- **Substrate reuse:** this *is* the autonomous-dev `exec_implement`→`git_verify`
  loop with worktree isolation, driven by the bench with a fixed
  `problem_statement` and a per-instance test command, and bounded by the existing
  turn / wall-clock / USD caps (F1a/F1b). No new engine.
- **⚠️ OS-resource isolation (roundtable BLOCKER B1):** F2 worktree isolation
  covers **file state only** — concurrent arm-C workers still share OS resources
  (TCP ports, temp dirs, package/import caches), so parallel runs interfere and
  inject non-reproducible noise into the very p95 wall-clock the claim rests on.
  S1 **must** add per-worker test-env isolation: a per-worker venv + a **port
  allocation table** (default), with a **real-docker-per-worker dry-run** in S1 as
  the gold-standard adequacy check. Without this the wall-clock numbers are not
  reproducible.
- **Worker prompt assembly lives in the harness, not the supervisor.** Assembling
  a worker's prompt in the supervisor would bill those tokens to the primary and
  erode the reduction; the harness owns prompt assembly (free). If the supervisor
  ever contributes prompt text, those tokens are accounted to the primary. Validate
  in S1.
- **In-loop test execution vs official grade — kept strictly separate.** The
  *in-loop* test command is the worker's iteration signal and may run in a
  lightweight per-repo venv or the instance's own Docker image; it is **advisory**.
  The **only** thing that decides `resolved` is the **official SWE-bench Docker
  grader on CT 101**, run once at the end on the emitted patch. This prevents the
  benchmark from grading itself. (Environment for in-loop tests on 2017-era repos
  is the top design risk — see Risks R1.)
- **Patch extraction is free:** the harness owns the worktree cwd, so the patch is
  `git diff <base_commit>` directly — no new server command, same as PR #986's
  integrated-cwd approach.

### Arm A — agentic solo

The **primary** runs the loop above, transported through a headless entrypoint
that bills `token_audit` with **`delegation_id` EMPTY** (candidate: `/v1/runs` or
`agent_shell`; **never Claude-Code**, which bypasses `token_audit`). The primary
is the model under test — **swappable** between the aimee primary (Claude) and
**gpt-5.5/codex** so we can run a **direct** head-to-head against Reddit's GPT-5.5
solo baseline. Its full multi-turn spend (explore + edit + test-reading + iterate)
is the A denominator.

### Arm C — agentic supervised

- **Workers** (free fleet + `gpu-gemma4` local) each run the loop in their own
  worktree with **tools enabled** (this is the key difference from PR #986's
  `--no-tools` single-shot). Worker tokens are $0 and excluded via
  `delegation_id`.
- **Supervisor loop (the primary):** across turns it (a) reads each worker's
  **turn digest** (bounded template — never raw code), (b) **redirects** a worker
  that is off-track with a short instruction, (c) on worker failure, optionally
  **escalates** to read code / take over (logged), and (d) at the end **selects
  best-of-N** by reviewing the short candidate diffs. Only these supervisor turns
  bill the A-equivalent (primary) tokens for arm C.
- **best-of-N** is a crossed factor (N ∈ {1, 2, 3}); N=1 is the "pure supervision,
  no selection" floor.

### Metrics recorded per (instance, arm, primary-model, N, compaction)

| field | source |
|---|---|
| `primary_in`, `primary_out` | `token_audit`, `delegation_id` EMPTY, session-scoped |
| `worker_tokens` | `token_audit`, `delegation_id` NON-EMPTY (honesty; $0) |
| `wall_clock_s` | `t1 − t0`, per instance |
| `resolved` | official SWE-bench Docker grader (CT 101) |
| `turns`, `api_calls`, `escalations` | loop instrumentation |
| `cost_usd` | primary priced; worker $0 |

### Reporting (extends `supervised_report.py`)

- **Headline:** primary-token reduction `1 − sum_C/sum_A`, split in/out, aggregated
  `sum(A)/sum(C)` (**not** mean-of-ratios), with **bootstrap CIs** over instances.
- **Wall-clock panel:** per-instance median/p95 for A and C; the A→C ratio; the
  claim gate requires ratio ≤ 1.0. Fleet throughput reported separately, labeled.
- **Resolution parity:** resolved/attempted **and** resolved/total (both
  denominators), per arm.
- **Honesty panel:** total-LLM tokens (primary + worker) A vs C — makes explicit
  that arm C spends *more total* compute, just far less *of the expensive
  primary's*.
- **Reducer/compaction lever:** because arm C is now **multi-turn**, the economizer
  / tool-output condensation / context-fold reducers **actually apply** here
  (they did **not** in PR #986's single-turn arm — measured 0 avoided tokens).
  This benchmark is the first place that lever can be shown paying off, so it is a
  **crossed factor** (reducers on/off).
- **Pareto panel:** plot every (arm × N × reducers) configuration over (primary
  tokens, wall-clock p95, resolution) to show dominance relationships — the single
  most informative view for the claim.
- **Failure-mode taxonomy:** a per-arm breakdown column — worker dropout / wrong
  diff / supervisor mis-selection / grader flake / environment issue — so a low
  arm-C resolution is attributable to a *cause*, not left ambiguous between "harness
  bug" and "genuine gap".
- **Stability panel:** run-to-run variance across the K repeats + p99 wall-clock,
  so tail behaviour and reproducibility are visible.

## The full benchmark suite

Fully-crossed where cheap, sampled where expensive. Temp = 0; **K ≥ 3** repeats
for variance; report CIs.

**Benchmark 1 — Reddit-10 head-to-head (the public claim).**
The *exact* 10 SWE-bench Lite instances from the Reddit post
(`pytest-11143`, `scikit-learn-13439`, `sympy-20212`, `django-12908`,
`pytest-6116`, `django-13447`, `django-15814`, `django-11179`, `sympy-13480`,
`scikit-learn-13584`). Primary = **gpt-5.5** for a like-for-like number; also run
with the aimee primary (Claude) for the aimee-native number. Arms A vs C; N ∈
{1,3}; reducers off. **This is the only benchmark whose numbers back the public
"beats −75.5% at no wall-clock penalty" claim.**

**Benchmark 2 — Held-out Lite sample (generalization).**
~25–30 Lite instances sampled **wide across the four repos** (django / sympy /
scikit-learn / pytest) plus a few others, disjoint from the Reddit-10. Same arms.
Guards against over-fitting the claim to 10 cherry-picked tasks.

**Benchmark 3 — Parallelism sensitivity (the wall-clock story).**
Sweep concurrent-worker count / fleet width; show the A→C wall-clock ratio as a
function of parallelism. This is the plot that visually kills Reddit's 3.75×: one
serial worker (their regime) vs N concurrent workers (ours).

**Benchmark 4 — best-of-N ablation.**
N ∈ {1,2,3}. Shows the resolution-vs-token trade of selection: N=1 is cheapest
but leans on worker reliability; higher N recovers resolution (PR #986 found the
limiter was worker reliability on long prompts, **not** selection). Also report
**oracle-best-of-N vs actual-best-of-N** — would selection-from-short-diffs pick
the diff that actually resolves? The gap quantifies whether N>1 is real selection
skill or just variance (the supervisor must pick from digests, not full code).

**Benchmark 5 — Reducer lever ablation.**
Reducers on/off over Benchmarks 1–2. First measurement of the multi-turn
economizer lever on a real agentic loop (net-of-recovery per the unified-economizer
telemetry).

**Benchmark 6 — Worker-pool comparison.**
Free-fleet workers vs GPU-local `gpu-gemma4` (Gemma-4-26B) — aimee's direct
counter to "local Qwen3.6-27B on a 3090". Reports whether a single local GPU
worker reproduces the token win (Reddit's regime) and how much the free-fleet
breadth adds on wall-clock.

**Optional stretch — Lite-300.** Full Lite if the fleet + grader throughput and
budget allow; reported as a generalization ceiling, not required for the claim.

## Build order (slices)

Each slice is independently roundtable-reviewable and lands testable.

- **S0 — Transport verification matrix (BLOCKING).** Submit a known **multi-turn
  agentic** loop through each candidate primary entrypoint (`/v1/runs`,
  `agent_shell`) and each worker dispatch, and assert the `token_audit` rows land
  with correct `delegation_id` polarity. Ship as a bench test that must be green
  before any graded run. *Resolves the Q1 blocker for the multi-turn case.*
- **S1 — Per-worker agentic harness.** Wire the autonomous-dev implement→verify
  loop + F2 worktree isolation into a bench-drivable per-instance runner:
  provision workspace at `base_commit`, run bounded explore/edit/test/verify,
  emit `git diff` patch. Includes the in-loop test-command resolution per repo
  (R1).
- **S2 — Arm A agentic-solo runner.** Primary drives the S1 loop headless via the
  S0-verified transport; captures per-instance `t0/t1` + primary tokens. Primary
  model swappable (Claude | gpt-5.5).
- **S3 — Arm C supervision loop.** Turn-digest template + supervision budget +
  redirect + escalate (logged) + best-of-N selection over the short diffs. Enforce
  the "no raw code unless escalate" discipline in the harness, not just by
  convention.
- **S4 — Metrics + reporting.** Extend `supervised_report.py`: per-instance
  wall-clock median/p95, agentic token split, two denominators, honesty panel,
  reducer-lever and best-of-N factors, bootstrap CIs.
- **S5 — Suite execution + grading.** Run Benchmarks 1–6 on .254 fleet + CT109 GPU;
  grade all patches with the official Docker harness on CT 101 (one prediction
  file per (arm, model, N, cmp), dedupe by `instance_id`). **Grader-retry policy:**
  max 2 deterministic retries on first failure; log the grader invocation count and
  **flag any instance whose resolution flips across the K repeats** — grader flake
  must not be conflated with run noise. Produce the report artifact.
- **S6 — Runbook + public-claim gate.** Rewrite `SUPERVISED_SWEBENCH.md` for the
  agentic variant; add a **fail-closed, multi-criterion claim gate**. The public
  "beats −75.5% at no wall-clock penalty" line is emitted **only if ALL** hold,
  under the official grader, on **both Benchmark 1 (Reddit-10) AND Benchmark 2
  (held-out)** — divergence between them withholds the claim (anti-cherry-pick):
  1. **Token reduction:** BCa-95% CI lower-bound of primary-token reduction > 0,
     anchored on **N=1** (best-of-N=3 numbers reported separately, never in the
     headline — N>1 is an asymmetric arm-C advantage).
  2. **Wall-clock:** **p95** A→C ratio upper-bound ≤ 1.0 (gate on p95, not median;
     a hung-worker tail must not hide behind the median). Computed against
     **aimee's own arm-A time**, not Reddit's 22.8 min (that is a cross-harness
     number, reported only as external context).
  3. **Resolution floor:** `resolved_C/total_C ≥ max(0.7 × resolved_A/total_A,
     0.25)` — without this, arm C could satisfy token+wall gates at ~0% resolution
     and the claim would be dishonest. Denominator is resolved/**total**.
  Statistics: **K=10** repeats for Benchmark 1 (the public claim), K=3 for
  ablations; **BCa 95% bootstrap CIs**. The pass/withhold decision is reviewed by
  **at least one party who did not author the harness or worker code**
  (anti-self-confirmation), and the full report is **published regardless of gate
  outcome** (e.g. "−78% primary tokens at 1.05× p95 wall-clock — claim withheld").
  A **secrets-leak scanner** (token-shape deny-list) over every emitted patch is a
  precondition for any public write-up.

## Risks / open questions

- **R1 — In-loop test environment for 2017-era repos (top risk).** The worker's
  iteration signal needs to *run tests* inside old repos where host python is too
  new and the shim-docker mangles stock images. **Recommendation (firmed by the
  roundtable): (a)** run in-loop tests inside each instance's official SWE-bench
  Docker image on a real-docker CT, **with explicit throughput planning** since CT
  101 is shared with grading and would bottleneck if both need real Docker
  simultaneously; **(c)** make in-loop tests optional (iterate on `git_verify` +
  build) as the fallback for repos whose image is too heavy or when CT 101 is
  saturated. (b) a pinned per-repo venv is the middle option. Locked at S1.
- **R2 — Worker reliability on long agentic prompts.** PR #986 found the limiter
  was worker reliability at 64k prompts (minimax "no content", 401s), not
  selection. Multi-turn loops are longer still. Mitigations: best-of-N,
  circuit-breaker skip, and the reducer lever (Benchmark 5) shrinking per-turn
  context. Report worker failure/dropout rate as a first-class metric, with a
  **quantitative invalidation threshold: >30% worker dropout on a benchmark = that
  run is invalid** (not silently averaged in).
- **R3 — Supervision cost blow-up via escalation.** If failures force the primary
  to read code often, arm C's token win erodes. The escalation log quantifies this;
  if escalation dominates, that is a *real, reportable* finding (the claim is
  workload-dependent), not something to hide. **Quantitative gate: >40% of arm-C
  primary tokens originating from escalation = claim withheld** (the win came from
  digging in, not supervising).
- **R4 — Model × architecture confound.** "Claude solo vs gpt-5.5-supervised" mixes
  two variables. Control: run the *same* primary model across A and C within each
  benchmark; the gpt-5.5-across-both cut in Benchmark 1 is the clean Reddit
  head-to-head.
- **R5 — Contamination.** SWE-bench Lite is public and may be in training data for
  both the primary and the workers. This affects *all* SWE-bench numbers including
  Reddit's; note it as a shared caveat rather than a differential advantage.
- **R6 — Fleet/grader capacity + cleanup.** Graded runs consume CT 101 docker,
  .254 fleet keys, and CT109 GPU; prior runs left venv/image/clone scratch. S5
  must budget throughput and own teardown (the cleanup debt from the PR #986 run
  is documented in memory).

## Roundtable review (2026-07-05) — adopted revisions

Reviewed by the `.254` fleet panel (6/7 participants, 54 deduped findings; 2
blocking). All load-bearing findings were **adopted** and folded into the sections
above; recorded here for provenance.

**Blockers (both adopted, now gating S0/S1):**
- **B1 — OS-resource isolation.** F2 worktrees isolate file state only; concurrent
  workers share ports/temp/caches → non-reproducible p95. → per-worker venv + port
  table + real-docker-per-worker dry-run in S1 (see Design + R6).
- **B2 — multi-turn token-attribution leaks.** cached-vs-uncached split, worker
  content billed to primary, and primary-side tools bypassing `delegation_id`. →
  S0 must assert all three; hard tool allowlist + uncached-headline denominator +
  thinking/text split (see Measurement hazards 4–5).

**Honesty-critical suggestions (adopted into the claim gate, S6):** resolution
floor `resolved_C/total ≥ max(0.7×resolved_A/total, 0.25)`; gate on **p95** not
median; anchor headline on **N=1**; must pass **both** Benchmark 1 **and** 2;
**K=10** + BCa-95% CIs for the public claim; within-aimee wall-clock ratio (not vs
Reddit's cross-harness 22.8 min); **independent** gate reviewer + **publish
regardless of outcome**; secrets-leak scanner precondition.

**Other adopted:** queue-vs-work t0 pinning + warmup discard; supervisor
context-drift cap + per-turn token curve; harness-owns-prompt-assembly; grader
retry/flip-flag; Pareto + failure-mode + stability panels; oracle-vs-actual
best-of-N; quantitative R2/R3 invalidation thresholds; firmed R1 recommendation.

**Not adopted / deferred:** cross-system literature anchoring and inlining the C6
notes (nits — the write-up will cite external results at publication time, out of
scope for the harness design).

## Deliverables

1. Extended `bench_swebench_supervised.py` (agentic arm A + arm C runners),
   `swebench_supervised_prep.py` (unchanged instance sets), `supervised_report.py`
   (agentic reporting), and a new per-worker agentic harness module.
2. The S0 transport verification-matrix bench test (green-gate).
3. A report artifact covering Benchmarks 1–6 with CIs, both denominators, and the
   honesty panel.
4. Rewritten `SUPERVISED_SWEBENCH.md` runbook + the fail-closed public-claim gate.
5. A short public write-up **only if** the claim gate passes.

---

*Convention: roundtable-review the CODE (not just this design) before each slice's
PR; grade only on the official SWE-bench Docker harness; aggregate `sum(A)/sum(C)`;
never post the single-shot (#986) number as a Reddit head-to-head.*
