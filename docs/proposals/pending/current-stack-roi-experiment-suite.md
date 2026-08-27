# Proposal: Current-stack ROI experiment suite

- **State:** PENDING. The staged design and a bounded local-Qwen calibration
  pilot are implemented; this PR makes no confirmatory ROI claim.
- **Date:** 2026-08-26
- **Owner:** benchmark and product-evidence maintainers
- **Baseline reviewed:** `origin/testing` at
  `6a1b61a99c9cac5273ccf6c26d2a6a185a6985bd`
- **Historical comparison pin:**
  `4b46f973a5a6c2b21c95ce4db9b4465fdfc92b47`
- **Related:**
  [Agentic supervised SWE-bench](agentic-supervised-swebench.md),
  [Standing benchmark cadence](standing-benchmark-cadence.md),
  [Capability-thresholded routing](../done/capability-thresholded-delegate-routing.md),
  [Economizer module](../../modules/economizer.md), and
  [provider-specific economizer safety](../done/provider-neutral-economizer-safety-spec.md)

## Decision

Build a staged, preregistered experiment suite whose headline metric is **total
settled provider cost per resolved task**. The denominator comes from an
independent task grader. The numerator includes every model call made for that
task: primary, manager, worker, selection, retry, recovery, recall, and any
model-backed judge.

The suite answers five different ROI questions rather than compressing them into
one flattering percentage:

1. How much did the current Aimee stack improve over the stack used for the
   July cost experiment?
2. What does each current economizer lever save on work that can actually
   trigger it, net of cache effects and recovery?
3. What do economization and delegation save separately and together when all
   worker costs and correctness are counted?
4. How quickly does durable organizational learning amortize across recurring
   work and a model change?
5. What fixed infrastructure and operator cost must those marginal savings pay
   back?

No result may use frontier-token displacement, estimated avoided tokens, or a
reduction ledger as a substitute for total realized cost per correct outcome.
Those remain useful diagnostic panels.

## Execution status

The first bounded pilot is recorded under
`benchmarks/results/roi/`. It exercises the production Go economizer handler
in-process against paired direct Qwen3.8 calls and validates the result schema,
budget preflight, activation trigger, byte-identical off path, provider-usage
reconciliation, and exact-answer grading. It is intentionally labelled
`pilot_only`: the module bus, recovery loop, delegation, natural coding tasks,
and a billable provider are not yet in its execution boundary.

## Why the existing results are insufficient

The tracked July cost campaign is useful historical evidence, but its public ROI
boundary is narrow:

- its 50-task headline counts only the frontier manager's prompt and completion
  tokens;
- worker-model usage is excluded;
- the tasks were single-shot, so the economizer recorded zero avoided tokens and
  could not exercise multi-turn folding, stable-prefix reuse, command-aware tool
  condensation, spill recovery, or net-of-recovery accounting; and
- task correctness was not graded.

The later E6 paired code-intelligence run corrects the quality problem for eight
tasks. It uses hidden tests and complete provider usage streams, but it measures
retrieved code context, not the economizer or the modern delegate stack.

The current stack differs materially from the July pin. Since that run, Aimee
has landed the Go economizer module and live ChatGPT and gateway fold paths,
cache usage telemetry, reducer-state ownership, capability-thresholded routing,
the staged Go delegate execution path, corrected tool-call handling, prompt
budgets derived from model capacity, and less flaky token-audit settlement. The
experiment must pin the exact current commit and prove that each configured
lever actually fired. A tier name in a config file is not evidence of
activation.

## Claim and accounting contract

### Primary estimand

For experimental condition `c`:

```text
total_realized_cost(c)
  = sum(settled marginal cost of every provider call attributed to c)

cost_per_resolved_task(c)
  = total_realized_cost(c) / officially resolved tasks(c)
```

A condition with no resolved tasks has infinite cost per resolved task. It may
not disappear as a missing value.

The primary comparison is the paired ratio:

```text
ROI cost ratio = cost_per_resolved_task(treatment)
               / cost_per_resolved_task(control)
```

A ratio below one favors Aimee. A result becomes a public savings claim only
when the preregistered confidence and quality gates below pass.

### Required token and cost buckets

Every provider call records, without merging differently priced categories:

- uncached input tokens;
- cache-read input tokens;
- cache-write input tokens and time-to-live class when the provider exposes it;
- output tokens, with reasoning output identified but not counted twice;
- resolved billable model, provider, endpoint, service tier, and pricing
  generation;
- actual settled marginal cost in the configured billing currency;
- task, condition, repeat, session, delegation, parent-call, and attempt IDs;
- success, denial, provider failure, timeout, disconnect, and indeterminate
  settlement state.

The run artifact reconciles the sum of the buckets to the provider usage object
and the immutable pricing record. Calls made under a subscription or known-free
local model still retain tokens and runtime, while marginal provider cost is
labelled according to the operator's actual contract. Unknown pricing blocks a
dollar claim; it never silently becomes zero.

### Secondary metrics

- provider tokens per resolved task, reported as a vector of the buckets above;
- task resolution and paired regressions/recoveries;
- median and p95 wall time per resolved task;
- frontier-model tokens displaced, always labelled separately from total cost;
- economizer baseline/reduced/removed tokens, activation reason, cache decision,
  spill bytes, recovery calls, recovery tokens, fallback, and bypass reason;
- primary, manager, and each worker's separate cost;
- CPU time, peak memory, storage growth, network bytes, and operator minutes.

`usage_kind=avoided`, reducer deltas, byte estimates, and hypothetical prices are
counterfactual diagnostics. They never enter `total_realized_cost`.

## Experimental controls

All provider-backed experiments share these controls:

1. Freeze Aimee commit, module image digests, task corpus hash, prompt and tool
   schema, model snapshots, reasoning level, output limits, maximum turns,
   provider endpoint, service tier, pricing generation, and grader version in a
   signed run manifest.
2. Use one clean worktree or workspace image per task and condition. No patch,
   transcript, reducer state, provider cache key, or Aimee memory crosses a
   condition unless the experiment explicitly studies carry-over.
3. Randomize paired condition order within task and repeat using a committed
   seed. Use both `AB` and `BA` order so provider load and time trends do not map
   onto one condition.
4. Give paired cells the same task information and tool authority. The only
   difference is the named experimental factor.
5. Count product-path retries and fallbacks as real work. Retry a harness or
   grader failure only under a deterministic preregistered rule, retain the
   failed artifact, and never retry a valid task failure.
6. Grade patches or answers without condition labels. SWE-bench uses the
   official Docker grader as the sole correctness authority.
7. Run a small calibration set before power analysis. Calibration tasks cannot
   enter the confirmatory result.
8. Cluster inference by task. Repeated stochastic runs of one task are not
   independent new tasks.

Temperature zero does not remove provider nondeterminism. Confirmatory cells use
at least three repeats, with the final repeat count set prospectively from pilot
variance and the smallest material effect.

## Prerequisite G0: accounting and activation proof

No paid benchmark starts until a no-claim readiness gate passes.

### Attribution

- Issue one synthetic call through each planned ingress and execution path:
  direct primary, Aimee primary, manager, every worker provider, retry, recall,
  and model-backed judge if used.
- Require a stable experiment `run_id` and task lineage on every ledger row.
  Global `MAX(id)` windows are forbidden because concurrent traffic can enter
  them.
- Reconcile every provider usage object to exactly one settled ledger record.
  Missing, duplicate, late, or unattributed rows fail the gate.
- Prove that a worker using the same billable model as the manager remains
  distinguishable by lineage rather than by model-name filtering.

### Economizer activation

- Record the resolved mode and each concrete lever sent to the economizer
  module.
- Require module availability at cell start and end.
- Require an explicit activation, no-gain, unsupported-provider, fallback, or
  bypass record for every eligible turn. Silence is a failed cell.
- For an enabled profile, include a planted trigger that must produce a known
  activation before any outcome task is admitted.
- Verify off mode forwards byte-identical provider-bound input and emits no
  economizer mutation.

### Budget

Before any provider dispatch, the runner prints and persists:

```text
expected spend = sum(expected calls by billable model × pilot token buckets × pinned rates)
hard maximum   = sum(call caps by billable model × pinned rates)
```

The run requires an explicit `--budget-limit` no lower than expected spend and
aborts before the next call when the remaining hard maximum does not fit. The
budget covers all models and retries, not only the primary. Calibration, pilot,
and confirmation have separate caps. This proposal does not estimate dollars
because no deployment pricing generation has been selected yet.

## Experiment E1: historical continuity and modern-stack effect

### Question

Would the same workload cost less and resolve more often on the current stack
than on the stack that produced the July result?

### Design

Run the exact tracked SWE-bench Lite 50 instance list against two isolated
stacks on the same days:

- `legacy`: Aimee
  `4b46f973a5a6c2b21c95ce4db9b4465fdfc92b47`;
- `current`: the confirmatory `origin/testing` pin.

Keep the provider model snapshots, task fixtures, tool authority, turn and token
budgets, grader, host class, and task order identical. Start with economizer hard
off in both stacks to isolate modernization. Cross topology as a second factor:

| factor | values |
|---|---|
| stack | legacy, current |
| topology | solo, delegated `N=1` |
| repeat | pilot `K=2`; confirmation selected by power analysis |

`N=1` is the headline because best-of-N spends asymmetrically for the delegated
condition. A separate `N=3` sensitivity panel is allowed but cannot replace it.

Both stacks must emit the new experiment result schema. A compatibility adapter
may read the legacy ledger, but it may not infer missing worker usage or success.
Run the legacy stack inside a disposable, credential-minimal environment with
network access limited to the required provider endpoints.

### Interpretation

This is the only experiment that attributes an overall difference to the modern
stack. If the legacy stack cannot meet attribution, tool-contract, or grader
parity, publish the incompatibility and treat the current rerun as a new
baseline. Do not compare its percentage causally with the historical number.

## Experiment E2: economizer lever ablation

### Question

Which current lossless economizer levers reduce total cost per correct task, and
does the saving survive recovery and cache settlement?

### Workload

Use at least 24 held-out, multi-turn tasks stratified before execution:

- verbose passing and failing test runs;
- compiler and linter diagnostics;
- large fresh JSON tool results;
- long histories that cross the fold threshold;
- tasks that need an identifier from the folded region; and
- tasks intentionally requiring one full-output recovery.

Single-shot tasks are an explicit negative-control stratum, not the main corpus.
They should show little or no folding gain.

### Conditions

Use actual dispatches in a cumulative lossless matrix:

| condition | JSON compaction | tool condensation | history fold/freeze | purpose |
|---|---:|---:|---:|---|
| `O` | off | off | off | byte-identical control |
| `J` | on | off | off | fresh structured tool result |
| `JT` | on | on | off | command-aware condensation |
| `JF` | on | off | on | fold, Coordinate Closet, and stable prefix |
| `JTF` | on | on | on | full lossless economizer |

If production configuration cannot independently express a cell, add a
benchmark-only resolved-profile input at the existing module request seam. It
must use the same production code path and be impossible to enable from ordinary
deployment configuration.

Aggressive lossy compression is exploratory and reported in a separate panel.
It cannot support the default ROI claim without an independently approved
semantic and outcome contract.

### Required panels

- actual cost and token buckets per resolved task;
- cache reads and writes by turn, including time-to-live;
- fold boundary reuse and cold re-folds;
- raw, condensed, spilled, and recovered bytes;
- recovery incidence and the provider tokens spent after recovery;
- no-gain, unsupported, fallback, and circuit-breaker rates;
- task regressions and recoveries by stratum.

Measure-only forecasts are compared with the paired actual off/on difference to
calibrate the estimator, but never substitute for it.

## Experiment E3: economizer and delegation factorial

### Question

Do economization and capability-routed delegation still save money when combined,
after every worker call and correctness outcome is counted?

### Design

Run a randomized `2 × 2` factorial on a held-out, officially graded agentic
SWE-bench set, reusing the isolation and grader contracts in
[Agentic supervised SWE-bench](agentic-supervised-swebench.md):

| condition | economizer | topology |
|---|---|---|
| `SO` | off | solo frontier agent |
| `SE` | full lossless | solo frontier agent |
| `DO` | off | capability-routed delegate, `N=1` |
| `DE` | full lossless | capability-routed delegate, `N=1` |

The primary model, task budget, tools, and final grader are fixed across cells.
The routed condition records the candidate set, capability threshold, chosen
provider/model, quoted marginal price, fallback path, and why each cheaper model
was admitted or rejected.

The report decomposes:

- economizer effect in solo work: `SE - SO`;
- economizer effect in delegated work: `DE - DO`;
- delegation effect without economization: `DO - SO`;
- delegation effect with economization: `DE - SE`; and
- the economizer/delegation interaction.

The sole headline is `DE` versus `SO` on total settled cost per resolved task.
Primary or frontier token displacement is a secondary chart. All worker tokens
and costs remain in the headline numerator, including failed candidates. Run
`N=3` only as a prespecified quality/cost frontier after the `N=1` result.

## Experiment E4: organizational-memory amortization

### Question

How many related tasks does it take before preserving reviewed work costs less
than rediscovering it, including the cost of retrieval and review?

### Design

Build held-out task families with four ordered episodes:

1. discover a repository, policy, or operational fact;
2. encounter a related task where that fact is useful but not sufficient;
3. encounter a correction or expiry that makes blind reuse unsafe; and
4. hand the next related task to a different model or tool.

Use at least three domains: repository conventions, incident diagnosis, and an
operational policy. Pair two conditions:

- `cold`: start every episode with an empty Aimee assertion, outcome, and
  retrieval scope;
- `learning`: allow only evidence produced in earlier episodes of that family,
  with the normal review, promotion, correction, and scope path.

Both conditions use the full lossless economizer so E4 isolates learning rather
than context reduction. The task author freezes each family before any run and
proves the useful fact is discoverable from source in both conditions. No task
answer may be preloaded as memory.

### Outputs

- cumulative settled provider cost after episodes 1 through 4;
- the first episode where the learning curve crosses below cold rediscovery;
- tokens and wall time spent retrieving, reviewing, correcting, and applying
  memory;
- successful transfer to the replacement model/tool;
- stale or wrong-memory use, abstention, correction latency, and scope leakage;
- operator review minutes.

The result is an amortization curve, not one aggregate percentage. A learning
condition that saves tokens only by applying an incorrect old fact fails.

## Experiment E5: known-mistake recurrence

### Question

Does a reviewed outcome prevent a known failure cheaply without blocking valid
work?

### Design

Create paired sequences around planted, non-destructive failure modes such as an
unsafe deployment order, a repository-local test omission, a forbidden credential
path, and a known-incompatible implementation approach.

In episode one, every condition encounters the same failure and correction. In
later episodes:

- `reset` discards the learned outcome;
- `reviewed` admits it through Aimee's ordinary reviewed procedure or guardrail
  path.

Include matched near-neighbor tasks where the old prohibition should not fire.
Measure recurrence, false blocks, time and provider cost to safe completion,
human interventions, and audit completeness. The primary endpoint is cost per
safe successful task. Report failure recurrence and false-positive rate together.

Do not convert prevented failures into expected dollars without an independently
sourced incident probability and loss distribution.

## Experiment E6: fixed operating cost and payback

### Question

At what recurring workload does measured marginal savings exceed the cost of
running and governing Aimee?

### Design

Replay a fixed mixed workload under `SO` and `DE` while measuring:

- idle and loaded CPU time, peak and steady memory, disk growth, and network
  transfer for server, KB, economizer module, and delegate workers;
- indexing, compaction, spill retention, audit, backup, and model-serving cost;
- operator time for installation, upgrades, memory review, incident handling,
  and benchmark maintenance; and
- provider cost from the same settled ledger used in E1 through E5.

Keep measured quantities separate from valuation assumptions. Publish a
sensitivity table over operator hourly cost, hardware amortization, electricity,
hosted-model prices, task volume, and repeated-work share.

```text
monthly fixed cost
  = infrastructure + storage + energy + operator time

measured marginal saving per resolved task
  = control cost per resolved task - treatment cost per resolved task

break-even resolved tasks per month
  = monthly fixed cost / marginal saving per resolved task
```

If marginal saving is zero or negative, break-even is never. The report must say
so rather than emit a negative task count.

## Statistical and publication gates

The pilot estimates variance and failure modes only. It does not contribute
observations to the confirmatory set. Before confirmation, commit a manifest
containing the final corpus, sample size, repeats, randomization seed, exclusions,
minimum material effect, and budget.

For a public **lower cost per correct task** claim, all of these must hold on the
held-out confirmatory set:

1. The task-cluster bootstrap 95% confidence interval for the treatment/control
   cost-per-resolved ratio lies below `1.0`.
2. The point estimate is at least the preregistered material saving, initially
   proposed as 10 percent.
3. The lower 95% confidence bound for the paired task-success difference clears
   a preregistered non-inferiority margin, initially proposed as no worse than 5
   percentage points, and every regression is listed.
4. No task, provider call, worker, retry, recovery, indeterminate settlement, or
   zero-resolve cell is missing from its denominator.
5. p95 wall time, operator time, cache behavior, and infrastructure overhead are
   published beside the cost result even when they regress.

For an **economizer** claim, E2 must also show successful spill recovery, no lost
required identifier or tool pair, and no silent fallback. For a **delegation**
claim, E3 must include all worker cost. For a **learning** claim, E4 must pass the
correction, scope, and model-transfer checks.

Report confidence intervals and the per-task paired data. Do not stack
percentages from E2 and E3; use the directly observed `DE` versus `SO` result.

## Result artifacts

Each run produces an append-only directory:

```text
benchmarks/results/roi/<date>-<run-id>/
  manifest.json
  calls.jsonl
  tasks.jsonl
  economizer.jsonl
  infrastructure.jsonl
  invalidations.jsonl
  summary.json
  report.md
```

`manifest.json` contains every pin and budget. `calls.jsonl` is the reconciled
all-model settlement ledger. `tasks.jsonl` contains condition-blinded outcomes
and grader provenance. `economizer.jsonl` contains lever activation and recovery,
not prompt content. `invalidations.jsonl` retains every excluded or invalid cell
with its reason and superseding run. The report links to hashes for provider
streams that cannot be committed safely.

Raw artifacts are immutable. Corrections add a new result directory and point
back to the invalid result.

## Implementation slices

### S0. Result schema and lineage

Create `benchmarks/roi/` with a versioned schema, validator, pricing pin, budget
preflight, and provider-usage reconciler. Add experiment lineage to every current
primary, manager, worker, retry, and recovery ledger path. Eliminate global
ledger-ID windows from ROI reporting.

### S1. Activation and parity gates

Add the G0 synthetic matrix, economizer resolved-profile record, module
availability check, planted activation triggers, and byte-identical off-mode
fixture. Reuse `benchmarks/compaction-quality/cache-ab.sh` where applicable.

### S2. Current harness adapters

Adapt `benchmarks/coding/swebench_suite.py` and the official grading path to emit
the ROI schema. Extend `supervised_report.py` from primary-token reporting to
all-model settled cost without deleting the existing historical panel.

### S3. E1 and E2 runners

Add isolated stack deployment for the historical pin, the exact Lite-50
continuity manifest, multi-turn economizer fixtures, cumulative lever profiles,
recovery tasks, and condition randomization.

### S4. E3 factorial

Compose the agentic supervised runner, capability routing, economizer profiles,
all-worker accounting, `N=1` headline, and `N=3` sensitivity.

### S5. E4 and E5 sequence runner

Add sealed task-family manifests, per-condition memory namespaces, review and
correction events, model/tool handoff, near-neighbor false-positive tasks, and
cumulative-cost reporting.

### S6. E6 and claim gate

Collect infrastructure and operator-time inputs, generate the payback sensitivity
table, implement task-cluster intervals and non-inferiority checks, and fail
closed when any accounting, quality, latency, or provenance gate is absent.

### S7. Calibrate, preregister, run

Run the zero-provider G0 checks, then a separately budgeted calibration and
pilot. Commit the confirmatory manifest before launching the confirmatory cells.
Publish valid, invalid, null, and adverse results together.

## Non-goals

- This proposal does not turn a reducer's predicted delta into realized savings.
- It does not claim a current dollar price before a deployment pins its actual
  pricing generation.
- It does not use best-of-three as the primary delegation comparison.
- It does not treat local worker tokens as free merely because their marginal
  provider bill is zero; runtime and infrastructure remain in E6.
- It does not compare current results causally with July unless the legacy and
  current stacks pass the same-day parity gate in E1.
- It does not authorize aggressive lossy economization or change any production
  default.
- It does not merge this suite into a scheduled cadence before the staged budget
  and artifact-retention policy are approved.

## Acceptance criteria for implementation

- G0 proves complete run-ID attribution and exact provider-usage reconciliation
  for all planned call paths.
- The runner refuses unknown prices for dollar claims, missing model usage,
  silent economizer activation, over-budget calls, unpinned inputs, and zero-
  denominator summaries.
- E1 through E6 can be selected independently and resume without rerunning valid
  completed cells.
- Unit tests cover all formulas, attribution, duplicate and late settlement,
  budget refusal, randomization determinism, condition isolation, grader retry,
  zero-resolve behavior, confidence gates, and adverse-result rendering.
- A fake-provider end-to-end run exercises every artifact without network use.
- No provider-backed confirmation starts before its expected and hard maximum
  spend are printed, persisted, and explicitly accepted.
- `python3 scripts/check-proposal-links.py`, the relevant benchmark unit tests,
  and repository lint pass.
