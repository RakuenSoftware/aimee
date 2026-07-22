# Rounds-to-resume — make compaction quality measurable

## Why

Aimee compacts every long session at 80% pressure (`src/headers/session_compact.h`),
and has a registry for pluggable context engines
(`src/server/context_engine.c:21`). Neither is measured.

`src/tests/test_session_compact.c` tests pressure thresholds and array mechanics
(`:41-155`) — `test_pressure_ok/warn/compact`, `test_estimate_tokens_*`,
`test_compact_long_conversation` — all asserting token counts and array sizes.
The ~40 suites under `benchmarks/` contain no compaction eval at all. So we test
**when** to compact and never **how well** it went.

That gap is load-bearing in two directions:

1. **Every claim about compaction is currently an assertion**, including the
   plausible ones (see the graveyard below — two proposals died on this rock,
   because neither could be justified without knowing whether today's compactor
   actually loses anything).
2. **A plug-in point with no metric is unfalsifiable.** `context_engine.c`
   invites alternative engines but offers no way to say one is better, which
   makes the seam decorative.

The cheapest honest move is to measure the thing before proposing to fix it.

## What

**Rounds-to-resume**: after a compaction, how many turns until the agent issues a
tool call that makes new progress, rather than one that recovers state it already
had before the boundary.

The signal is detectable without an LLM judge. The loop already computes a tool
signature — `name:first-200-chars-of-args` (`src/posix/agent_runtime.c:1607-1610`)
— for the circuit breaker. A post-compaction call whose signature matches a
pre-compaction call is a candidate re-derivation.

**The narrowing that makes it correct:** a signature match is *not* sufficient.
An agent re-running `test` after a fix matches its own earlier `test` call, but
that is progress, not rediscovery. Only **read-only rediscovery** counts —
`read_file` / `grep` / `git_status` / `list_files` / `find_symbol` with identical
args — where re-issuing the call cannot be anything other than recovering context
lost at the boundary. Mutating and verifying calls (`edit_file`, `write_file`,
`bash`, `test`) are excluded by construction: repeating them is legitimate.

Scope:

- Instrument the existing compaction path; no behavior change, no new tool, no
  prompt change. Default-on for measurement, since it only counts.
- Report per-session: rounds-to-resume, count of read-only re-derivations, and
  the bytes the boundary retained.
- Commit a **baseline for today's compactor** before any compaction change is
  proposed. That number is the deliverable.

## What this unlocks

- Any future compaction change gets a falsifiable target instead of a story.
- `context_engine.c` gets its first objective engine-vs-engine comparison, which
  is what turns a plug-in point into an optimizable surface.
- If the baseline shows today's compactor loses little, that is a genuine result:
  it retires a whole class of proposals cheaply, including two below.

**Charter roles: Evaluate-Optimize / Calibrate.**

## Acceptance

- Rounds-to-resume is reported for today's compactor on the existing agentic
  harness and the number is committed to the tree as a baseline.
- A read-only call whose signature matches a pre-compaction call is counted as a
  non-progress round; a repeated `test` or `edit_file` is **not**. Asserted by a
  test that constructs both sides of a compaction boundary explicitly, including
  the repeated-`test` false-positive case.
- Zero behavior change: with the metric enabled, an identical session produces an
  identical message history and identical compaction boundaries.

---

## Graveyard

Recorded so these are not re-proposed. Each was scoped, checked against the code,
and rejected on evidence.

### Rejected — `command_run` macro tool

An ordered step list (`{steps: [{tool, args}], stop_on_error}`) dispatched
in-process, collapsing inspect→patch→build→test into one turn.

- **Batching already covers the fan-out.** Aimee executes every tool call in a
  response (`for (int i = 0; i < parsed.call_count; i++)`,
  `src/posix/agent_runtime.c:1376`, cap 16), and all three wire formats batch
  results back. Independent calls already cost one turn. The tool surface is not
  the waste.
- **The uncovered case is worth ~one turn.** Only the *ordered dependent*
  sequence is inexpressible in a parallel call array (no ordering, no
  short-circuit) — and a macro saves only the *acknowledgment* turn, where the
  model reads "edit succeeded" and says "now test." It does not save the turn
  after; the model still must see the test result and react.
- **And that saving is conditional.** It materializes only when the model
  pre-decides correctly. When it guesses wrong, the macro runs steps against a
  state that never existed and burns more tokens than it saved.
- **Against a permanent cost:** a tool, a schema, four registration sites
  (builtin table, `src/toolset.c:52` allowlist, dispatch chain) plus the Windows
  twin, and a partial-execution failure mode every future tool must reason about.
- **The workflow engine is not the alternative either.** A wfe block is an LLM
  agent (`wfe_live_delegate.c` dispatches `agent_execute_with_tools_for_role`);
  every producing block takes a locked git worktree (`wfe_blocks.c:200`), a
  `lifecycle_work_item` row, and a USD cap; runs land on a background scheduler
  thread (`wfe_scheduler.c:92`), so `workflow_run` returns a `work_item_id` to
  watch, not results. The step vocabulary is `ADVANCED / PENDING / FAILED /
  LOOPED` — routing signal, no payload; the nearest thing, a `command` custom
  block, discards stdout outright (`free(out)`, `wfe_blocks.c:1445`). A one-block
  workflow is still an agent plus a worktree plus a work item: it does not scale
  down to a tool call. `wfe_iface.h:75-77` records that the seam is "NEVER a
  free-form string, so the narrow seam cannot silently re-acquire a surface" — a
  payload channel was considered and deliberately rejected there.

### Rejected — checkpoint-anchored compaction

Write the durable-job cursor and snapshot id into the compaction summary as
machine-observed fact, so a resumed agent need not re-run tools to learn what it
already did.

- **It would be a no-op exactly where it matters.** `agent_set_durable_job()` has
  three non-test setters: `delegate --durable` (`src/cmd_agent_delegate.c:1392`),
  background/detached delegates (`src/server/server_compute.c:636`, gated on
  `background_job_id > 0`, assigned only at `:1785`), and `agent_job_resume()`
  (`src/server/agent_tasks.c:450`). An ordinary interactive session — CLI chat,
  webchat — passes through none of them, so `agent_get_durable_job_id()` returns 0
  and every read is skipped (`src/posix/agent_runtime.c:1654`). Interactive
  sessions are the long-running ones that actually hit 80% and compact; delegates
  start from empty message arrays and rarely live long enough.
- **So the mechanism targets the sessions that need it least.** The underlying
  idea — observed facts over prose recollection — might survive on a substrate all
  sessions have (`agent_tools_get_snap_id()`, the snapshot record), but that is a
  redesign resting on an unproven premise. Measure first: if the baseline above
  shows the compactor loses little execution state, this never needs building.

### Rejected — a backward-planning instruction block

An exec-context block telling the model to state the end-state and acceptance
check first, then derive prerequisites backward.

- **The guidance already exists, twice.** `src/prompts.c:36` (STANDARD) and `:73`
  (EXTENDED) both say "PLAN: Break complex tasks into discrete steps. State your
  plan."; EXTENDED then repeats a second "Reasoning Structure" planning block at
  `:93-104`. A third would duplicate the persona path — which delegates receive
  via `prompt_persona_text` (`src/persona.c:513`).
- **And it would conflict with the exec path**, whose `default_exec_instructions`
  (`src/server/agent_runtime.c:1383-1395`) say "Execute the task directly."
- **The originating claim disclaims itself.** The external source of this idea
  attributes its benchmark gains to system-level associations and explicitly
  declines to give "causal estimates for backward reasoning or any other
  individual feature." There is no evidence to import.
- *Adjacent finding worth its own small change:* EXTENDED mode carries two
  overlapping planning blocks (`prompts.c:73` and `:93-104`). That is prompt
  budget spent twice on one instruction — a cleanup, not a feature.

### Rejected — deeper per-directory convention discovery

Re-run `context_discover()` from the directories being edited rather than the
session cwd, so a nested `subdir/AGENTS.md` is found when a session starts at the
repo root.

- **The walk is correct and covered.** `test_finds_nearest_file_first`
  (`src/tests/test_context_discover.c:77-106`) places a root `AGENTS.md` and a
  nested `.aimee-rules`, calls `context_discover(sub, ...)`, and asserts both are
  found with the nested one first.
- **`AGENTS.md` is a foreign per-tool convention aimee holds at arm's length** —
  it is the per-tool mechanism for what aimee does globally, with provenance, via
  the memory system. Repo-file conventions are classified **untrusted advisory**,
  the lowest trust tier ([memory DB1/DB2 architecture](../done/memory-db1-db2-architecture.md),
  §210/§234), and memory interception deliberately excludes
  `AGENTS.md`/`CLAUDE.md`/`.cursorrules`
  ([central agent-memory interception](../done/central-agent-memory-interception.plan.md), §82):
  aimee reads these files, never writes them.
- **So widening the surface is the wrong direction.** It admits more untrusted,
  unprovenanced input to buy behavior the memory system owns. If per-directory
  rules must bind harder, the answer is `.aimee-rules` with real provenance
  through the memory contract — a different proposal, with a trust argument in it.
