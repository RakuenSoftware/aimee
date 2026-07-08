# Ensembles

An **ensemble** is a panel of agents collaborating on one task instead of a
single delegate. aimee runs ensembles in three modes that share one concept, one
config namespace (`ensemble.*` / `roundtable.*`), and one `aimee ensemble` verb —
and they **compose** (a delegate's output can feed a running session, below).

| Mode | Shape | Reach it via |
|---|---|---|
| **aggregate** (Mixture-of-Agents) | fan out one prompt to diverse models in parallel, one aggregator synthesizes a single answer | `aimee ensemble aggregate` · `aimee delegate aggregate` · method `delegate.aggregate` |
| **roundtable** | a multi-persona review/debate panel; multiple rounds, each round sees the prior one | `aimee ensemble roundtable` · `aimee delegate roundtable` · method `delegate.roundtable` · the workflow `gate.roundtable` block |
| **session** | a persistent, templated, turn-based session (code-review, debate, planning, design-critique) | the `ensemble_*` MCP tools · a delegate bound to a channel · `aimee ensemble session …` |

The first two are one-shot panels that return a synthesized artifact. The third
is a stateful, multi-phase conversation that lives in the DB and advances a turn
at a time. All three are "a group of agents working a problem together" — the
difference is parallel-and-synthesize vs. turn-by-turn.

> Naming: the delegate Mixture-of-Agents / roundtable subsystem
> ([`src/server/delegate_ensemble.c`](../src/server/delegate_ensemble.c)) and the
> templated session store ([`src/db1/ensemble.c`](../src/db1/ensemble.c)) are two
> expressions of the same "ensemble" concept, deliberately sharing the name and
> the `aimee ensemble` command, not a collision.

## aggregate — Mixture-of-Agents

Fan one prompt out to the reference panel in parallel; an aggregator folds their
answers into one. Weak or partial panels degrade to the best single candidate
rather than failing.

```bash
aimee ensemble aggregate "Design a migration plan for the auth schema"
# equivalent, back-compat alias:
aimee delegate aggregate "Design a migration plan for the auth schema"
```

Configured by `ensemble.*` (see [Settings](SETTINGS.md)):

- `ensemble.reference_models` — diverse model/agent names for the fan-out.
- `ensemble.reference_personas` — optional per-reference persona overrides.
- `ensemble.aggregator` — the agent that synthesizes the final answer.
- `ensemble.min_successful` — min references that must succeed before degrading (default 2).
- `ensemble.max_cost_usd` — optional per-run USD cap (0 / unset = no cap).

## roundtable — review / debate panel

Runs multiple rounds; each round's panel sees the prior round and returns the
best round's artifact. Each panelist runs **without file tools** and gets a
**distinct persona** (security, architect, QA, contrarian, constructive
reviewer), so it reviews the artifact you give it instead of wandering the
filesystem — and tool-less models (e.g. codex) can still participate.

```bash
aimee ensemble roundtable "Is this migration plan sound?" --mode review
#   --mode review|draft        review an artifact, or collaboratively draft one
#   --turns parallel|sequential  panel fans out at once, or speaks in turn
```

Panel + aggregator are reused from `ensemble.*`; the loop is tuned by
`roundtable.*` (`roundtable.max_rounds`, `roundtable.converge_threshold`,
`roundtable.deadline_ms`, `roundtable.turns`). The workflow engine's
`gate.roundtable` block ([Workflows](WORKFLOWS.md)) is the same panel used as a
pass/fail review gate inside a run.

Prefer `aimee ensemble roundtable --mode review` (or the `delegate.roundtable`
tool) for a multi-model review **gate**; use `aimee delegate review --via M` for
a single, exploratory, tools-on review of live code. See
[Delegates](DELEGATES.md).

## session — templated, turn-based

A persistent session where a template lays out phases and roles, and agents take
turns. Ships with `code-review`, `debate`, `planning`, and `design-critique`
templates; drop a project-local template in `ensemble_templates/` (the legacy
`session_templates/` path still resolves). State lives in the DB1 `ensembles`
table.

Agents drive sessions with the **MCP tools** `ensemble_start`, `ensemble_status`,
`ensemble_pause`, `ensemble_advance`, and `ensemble_list` (the `session_*` names
remain as hidden aliases). Each `ensemble_advance` records a turn and returns the
next expected participant and prompt.

```jsonc
// ensemble_start
{ "template": "code-review", "channel": "pr-482",
  "assignments": { "reviewer": ["claude-1", "gemini"], "author": ["me"] } }
```

`aimee ensemble session <start|status|pause|advance|list>` is the human-facing
counterpart; today it points at the MCP path (thin-client session control over
`/v1` is a pending enhancement).

## How the modes compose

A delegate that runs on a **channel** advances the session ensemble bound to that
channel with its output: after the delegate replies, aimee looks up the current
ensemble for the channel
([`db1_ensemble_find_current_by_channel`](../src/db1/ensemble.h)) and calls
`ensemble_advance` with the delegate's result as that turn. So an aggregate or
roundtable run can *be a turn* in a longer templated session — the one-shot panel
feeds the stateful one.

## See also

- [Delegates](DELEGATES.md) — the delegate core aggregate/roundtable run on.
- [Workflows](WORKFLOWS.md) — `gate.roundtable` uses the same review panel.
- [Personas](personas.md) — the reviewer personas panelists run as.
- [Settings](SETTINGS.md) — the `ensemble.*` / `roundtable.*` keys.
