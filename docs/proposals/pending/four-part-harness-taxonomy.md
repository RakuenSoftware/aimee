# Four-part harness taxonomy: name the parts, attribute the failures, delete the scaffolding

Status: pending
Owner: harness / observability
Scope: vocabulary + two measurement tools (one shipped runnable, one C port specced)

## 0. Summary

aimee *is* a harness — the runtime layer between the model and the world. It
intercepts every action through hooks and MCP, assembles context, routes
delegates, and gates writes. That machinery is spread across ~400 files in
[`src/`](../../../src) with no shared vocabulary for *which kind of thing* each
subsystem is. This proposal adopts one framing and builds two measurements on top
of it.

The framing: **every agent harness is made of exactly four parts.**

1. **Agent loop** — reason → call a tool → read the result → reason again. ReAct.
2. **Tool interface** — what the model may touch, and how calls are described,
   dispatched, and returned.
3. **Context management** — what enters the window each turn (write / select /
   compress / isolate).
4. **Control** — budgets, retry policy, circuit breakers, observability.

Two consequences drive everything below:

- **Failures are attributable.** When an agent misbehaves it is almost always one
  of the four, not "the model." Looped forever or quit early → loop/control.
  Wrong tool or mangled args → tool interface. Ignored what you gave it → context.
  Worked in the demo, melted in prod → control. We can *measure* this distribution
  instead of guessing it.
- **A good harness shrinks as models improve.** Every component encodes an
  assumption about what the model can't do on its own. As models get better, some
  of those assumptions expire and the component becomes pure context tax. We can
  put *delete-pressure* on the parts that bet against the model, while protecting
  the parts that don't.

This proposal ships:

- **A canonical vocabulary** (§1) and a **subsystem map** (§2) classifying every
  major aimee subsystem as one of the four parts and as a **durable bet** vs a
  **delete-pressure candidate**.
- **A failure classifier** (§3) — runnable today as
  [`scripts/harness/classify_failures.py`](../../../scripts/harness/classify_failures.py),
  with the in-process C port into [`trace_analysis.c`](../../../src/trace_analysis.c)
  specced here.
- **A delete-pressure metric** (§4) — runnable today as
  [`scripts/harness/delete_pressure.py`](../../../scripts/harness/delete_pressure.py),
  with the runtime hit-rate counter specced here.
- **An explicit defense of the durable bets** (§5): memory, KB, guardrails, and
  worktree isolation are *not* betting against the model and are exempt from
  delete-pressure.

## 1. The vocabulary (canonical names)

These four words become the standard way aimee talks about harness failures and
harness code, in `MANUAL.md`, in trace output, and in review.

| Part | One-line | Owns the question |
|------|----------|-------------------|
| **loop** | ReAct turn cycle | "did it keep going the right amount?" |
| **tool** | tool surface + dispatch | "was the right call made correctly?" |
| **context** | what's in the window | "did it use what it was given?" |
| **control** | budgets, limits, observability | "is it safe and bounded in prod?" |

The triage move, stated as a function: **symptom → part.**

| Symptom | Part |
|---------|------|
| Looped forever / quit early / stuck redoing | loop (or control, if no limit existed) |
| Called the wrong tool / mangled the args | tool |
| Ignored a fact, file, or instruction you clearly provided | context |
| Worked in the demo, melted under load / cost / concurrency | control |

## 2. Subsystem map

Each subsystem is one of the four parts, and is either a **durable bet** (provides
something the model structurally cannot do for itself — state, authority, economics)
or a **delete-pressure candidate** (compensates for a current model weakness and
should be re-tested, and likely trimmed, as models improve).

| Subsystem | Files (representative) | Part | Class |
|-----------|------------------------|------|-------|
| Turn execution / ingress translation | [`conversation.c`](../../../src/conversation.c), [`proxy_bootstrap.c`](../../../src/proxy_bootstrap.c), [`server`](../../../src/server) | loop | durable (wire-format bridge) |
| Retry / recovery loop detection | [`trace_analysis.c`](../../../src/trace_analysis.c) | loop | durable (observability) |
| MCP tool surface | [`mcp_tools.c`](../../../src/mcp_tools.c), [`toolset.c`](../../../src/toolset.c) | tool | durable (the bridge itself) |
| Per-tool prompt augmentation | [`src/tool_prompts/`](../../../src/tool_prompts), [`gen_tool_prompts.py`](../../../src/gen_tool_prompts.py) | tool | **delete-pressure** |
| Anti-pattern warnings | [`guardrails_semantic.c`](../../../src/guardrails_semantic.c), `db2/anti_patterns` | tool/context | **delete-pressure** (the advisory half) |
| Delegate router | [`cmd_agent_delegate.c`](../../../src/cmd_agent_delegate.c), [`learning_router.c`](../../../src/learning_router.c) | tool | durable (economics, not capability) |
| 4-tier memory | [`memory_core.c`](../../../src/memory_core.c), `memory_*.c` | context | **durable (state the model can't have)** |
| Knowledge base + curator | [`src/kb/`](../../../src/kb), `kb_curator_*` | context | **durable** |
| Context assembly / briefing | [`memory_assemble.c`](../../../src/memory_assemble.c), [`session_briefing.c`](../../../src/session_briefing.c) | context | durable mechanism, **delete-pressure** on its hand-tuned heuristics |
| Compaction | [`compact.c`](../../../src/compact.c) | context | durable (fights context rot) |
| Sensitive-file blocking | [`file_safety.c`](../../../src/file_safety.c), [`guardrails.c`](../../../src/guardrails.c) | control | **durable (authority, not capability)** |
| Git verify / branch ownership | [`git_verify.c`](../../../src/git_verify.c), [`branch_ownership.c`](../../../src/branch_ownership.c) | control | durable |
| Worktree isolation | [`workspace.c`](../../../src/workspace.c), [`worktree_gc.c`](../../../src/worktree_gc.c) | control | durable (blast radius) |
| Budgets / cost + success tracking | delegate status, `learning_router.c` | control | durable |

The single most important reading of this table: **aimee's delete-pressure
candidates are concentrated in `tool` and `context` scaffolding (tool prompts,
anti-pattern advisories, packing heuristics), while every `control` subsystem and
the memory/KB core are durable.** That is the correct shape — the harness is thin
where it doubts the model's intelligence and thick where it supplies state and
authority the model cannot have on its own. The job is to keep it that shape as
models improve.

## 3. Failure classifier

**Goal:** turn "context management is our biggest tax" from a hunch into a measured
distribution, automatically, from data aimee already records.

**Input:** execution-trace rows — the exact shape
[`trace_analysis.c`](../../../src/trace_analysis.c) already mines
(`db1_execution_trace_mining_row_t`: `plan_id, turn, direction, tool_name,
tool_args, tool_result`).

**Heuristics** (priority order; specific → general), reusing the constants and
error indicators already in `trace_analysis.c`:

- **loop — `retry-loop`:** same tool ≥ `RETRY_THRESHOLD` (3) consecutive on one
  plan with ≥ 2 errors. This is verbatim the rule `detect_retry_loops()` already
  implements; we relabel its output as a *loop* failure.
- **control — `runaway-length`:** turn count exceeds a cap with no budget/limit
  marker observed → the stop condition was missing.
- **control — `limit-marker`:** an error result naming a boundary (`429`,
  `rate limit`, `timeout`, `context length`, `budget`, `circuit`, …). Outranks
  everything: a 429 is never a tool-design bug.
- **context — `redundant-refetch`:** a `(tool, args)` call that already succeeded
  earlier in the plan is re-issued and *nothing had changed* → the prior result was
  in context and ignored. "Nothing changed" is made precise to exclude the
  legitimate read-edit-reread cycle: the signal fires when the re-issue **errors**,
  or when it **returns a byte-identical result** to the earlier success. A re-issue
  that returns a *different* result is a valid re-read after a mutation and is not
  flagged. (Known false positive: an idempotent poll loop returning identical bytes
  also matches; this is the lowest-confidence non-unclassified signal — a
  nomination, not a verdict.)
- **tool — `dispatch-or-arg-fault`:** an error result that is dispatch/arg-shaped
  (`command not found`, `unknown tool`, `invalid argument`, `usage:`,
  `missing required`, parse/schema errors) → the call was malformed or misdirected.
- **unclassified:** an error with no distinguishing signal. Surfaced, not dropped —
  hiding it would understate the tax.

**Shipped now:**
[`scripts/harness/classify_failures.py`](../../../scripts/harness/classify_failures.py)
implements this end to end with a `--self-test` (synthetic plan per part, red/green)
and reads `aimee trajectory export --json` or any trace JSON. This is the reference
the C port must match.

**C port (specced):** add `harness_classify_failure()` alongside `trace_mine()` in
[`trace_analysis.c`](../../../src/trace_analysis.c). Note the existing in-memory
`trace_row_t` keeps only `int has_error` and **discards `tool_result`**, so it is
insufficient: the `control`/`tool`/`context` sub-classification needs the raw
result string (for the boundary and dispatch markers and the byte-identical
refetch check). The faithful port must therefore classify off
`db1_execution_trace_mining_row_t` — which retains `tool_result` — or precompute the
marker bits at load time, rather than reuse the `has_error`-only buffer. It should
also lift the retry rule's bare `errors >= 2` into a named `RETRY_MIN_ERRORS`
`#define` (the Python keeps `RETRY_MIN_ERRORS = 2` explicitly) so the two stay in
lockstep symmetrically. Surface via
a `aimee trajectory classify [--json]` subcommand
([`cmd_trajectory.c`](../../../src/cmd_trajectory.c)) and persist the rolling
distribution so `aimee doctor` / [`cmd_diagnose.c`](../../../src/cmd_diagnose.c) can
report "this week's failures: 58% context, 19% tool, …". The evidence-ranked
diagnosis store (`DIAG_RANK_*`) is the natural place to attach a classified
incident as `evidence_for` a hypothesis.

## 4. Delete-pressure metric

**Goal:** make "delete harness code as models improve" a maintenance ritual with a
number, not a slogan. Focus first on the per-tool prompt augmentations in
[`src/tool_prompts/`](../../../src/tool_prompts) — the most literal "betting against
the model" surface, injected into context every single turn.

**Two signals, deliberately separated:**

- **Static (shipped):**
  [`scripts/harness/delete_pressure.py`](../../../scripts/harness/delete_pressure.py)
  scores each scaffold by per-turn token tax and *doubt density* (imperative
  hedges — "Always", "Never", "Avoid", "Do not" — each encoding "the model won't
  do this unless told"). On the current 8 scaffolds it reports ~207 tokens of
  fixed per-turn tax and ranks `run_background_process`, `list_files`, and
  `write_file` as the top re-test-then-likely-trim candidates. Static signals only
  **nominate**.
- **Runtime (authoritative, specced):** the real test is whether *removing* a
  scaffold changes an outcome. Two data sources:
  - **anti-pattern hit-rate** — `db2/anti_patterns` already has
    `db2_anti_pattern_bump()`. A pattern that has never bumped is dead weight. The
    tool already ingests this via `--anti-patterns export.json`; we need a
    `aimee guardrails anti-patterns export --json` that emits `{pattern, hits}`.
  - **scaffold A/B** — gate each tool prompt behind a flag and compare tool-call
    validity (the `tool` failure rate from §3) with and without it. A scaffold
    whose removal doesn't move the failure rate has expired.

**Guardrail on the metric:** delete-pressure ranks *candidates for re-testing*, not
deletions. Nothing is removed without the runtime signal confirming the model no
longer needs it. The static score points the camera; the A/B pulls the trigger.

## 5. The durable bets (exempt from delete-pressure)

These are not betting against the model's intelligence, so model improvement does
not erode them. They are explicitly out of scope for §4 and should keep getting
investment:

- **Memory + KB** (`memory_*.c`, [`src/kb/`](../../../src/kb)). A smarter model still
  has zero memory of *your* last session — that is a state problem, not an
  intelligence problem. This is aimee's strongest bet, and the framing validates
  it: state is the one part that doesn't evaporate as models improve. See
  [How aimee learns](../../KNOWLEDGE.md).
- **Guardrails / file-safety / git-verify / isolation** (`file_safety.c`,
  `guardrails.c`, `git_verify.c`, `worktree_gc.c`). These govern *authority and
  blast radius*, not capability. A more capable model that is *allowed* to clobber
  `.env` or another session's worktree is more dangerous, not less. Authority
  bounds should track trust and concurrency, never model IQ.
- **Delegate routing economics** ([`learning_router.c`](../../../src/learning_router.c)).
  Routing to the cheapest capable model is an economic bet that survives model
  improvement. Only the router's *complexity* is subject to pressure — the routing
  itself is durable.

## 6. Rollout

1. **Vocabulary** — land §1 in `MANUAL.md` and reference the four parts in
   trace/diagnose output. (cheap, unblocks the rest)
2. **Classifier** — ship the Python reference (done), then port to
   `trace_analysis.c` + `aimee trajectory classify`, then persist the rolling
   distribution into `aimee doctor`.
3. **Delete-pressure** — ship the static tool (done), add
   `aimee guardrails anti-patterns export --json`, wire `delete_pressure.py` into
   `make lint` as a non-failing report, then build the scaffold A/B harness.
4. **Quarterly ritual** — on each model upgrade, run delete-pressure, A/B the top
   candidates, and trim. The harness should be smaller after a model upgrade than
   before it.

## 7. Verification status

- `scripts/harness/classify_failures.py` — **verified**: `--self-test` is green
  (exit 0). Beyond one synthetic plan per part, it asserts the read-edit-reread
  cycle is *not* flagged, that a benign `room`-containing error is not mis-attributed
  to control (word-boundary marker matching), the erroring-refetch path, and the
  empty-trace / all-success / runaway-length edge cases. Each assertion fails loudly,
  so a degenerate classifier cannot pass.
- `scripts/harness/delete_pressure.py` — **verified**: `--self-test` green; the test
  holds word count fixed and varies only prescriptiveness (length and doubt are not
  confounded) and asserts substrings of doubt terms (`commonly`/`mustard`) score
  zero. A live scan of the real `src/tool_prompts/` reports the 8-scaffold inventory
  (~207 tokens/turn fixed tax).
- C port into `trace_analysis.c`, the `trajectory classify` / `anti-patterns
  export` subcommands, and the scaffold A/B harness — **design only, unverified.**
  Not built or run in this change.
