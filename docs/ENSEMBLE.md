# Ensembles

An **ensemble** is a panel of agents collaborating on one task instead of a
single delegate. aimee runs ensembles in three modes that share one concept, one
config namespace (`ensemble.*` / `roundtable.*`), and one `aimee ensemble` verb —
and they **compose** (a delegate's output can feed a running session, below).

| Mode | Shape | Reach it via |
|---|---|---|
| **aggregate** (Mixture-of-Agents) | fan out one prompt to diverse models in parallel, one aggregator synthesizes a single answer | `aimee ensemble aggregate` · `aimee delegate aggregate` · method `delegate.aggregate` |
| **roundtable** | a configured multi-persona panel; every seat analyzes once before finalization | method `delegate.roundtable` · the workflow `gate.roundtable` block |
| **session** | a persistent, templated, turn-based session (code-review, debate, planning, design-critique) | the `ensemble_*` MCP tools · a delegate bound to a channel · `aimee ensemble session …` |

The first two are one-shot panels that return a synthesized artifact. The third
is a stateful, multi-phase conversation that lives in the DB and advances a turn
at a time. All three are "a group of agents working a problem together" — the
difference is parallel-and-synthesize vs. turn-by-turn.

> Naming: the delegate Mixture-of-Agents / roundtable subsystem
> ([`src/modules/roundtable/delegate_ensemble.c`](../src/modules/roundtable/delegate_ensemble.c)) and the
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

- Saved roundtable seats are the complete panel. An omitted roundtable name
  resolves through `roundtable.default`, then the saved preset named `default`.
  A named/default preset may contain any supported number of seats; its exact
  count is authoritative and is never expanded from the eligible roster.
- When no saved roundtable is acquired, a direct aggregate/roundtable may use at
  most two available review agents, preferring different providers.
- `ensemble.reference_models` is retained as the preset's applied compatibility
  representation; it does not authorize an unconfigured panel larger than two.
- `ensemble.reference_personas` — optional per-reference persona overrides.
- `ensemble.aggregator` — the agent that synthesizes the final answer.
- `ensemble.min_successful` — min references that must succeed before degrading (default 2).
- `ensemble.max_cost_usd` — optional per-run USD cap (0 / unset = no cap).

## roundtable — review / debate panel

Runs one independent analysis per configured seat before finalization. Review
panelists get read-only Aimee index tools and a
diverse persona lineup (original-request alignment, security, architect, QA,
contrarian, constructive reviewer).

When a saved roundtable enables **Discussion mode**, the same successful seats
compare their independent reports once before deterministic synthesis. Nits,
suggestions, and ordinary blocking findings always stop after that one cycle.
A foundational finding also stops after one cycle when the seats agree or one
position already has a strict majority. Only an explicitly disputed
foundational finding may continue to another cycle, and subsequent cycles carry
only those contested stable issue IDs. Discussion ends when a strict majority
forms; deadline or quorum loss parks the workflow visibly rather than fabricating
consensus. Deterministic synthesis retains findings unless a strict majority
rejects them.
The denominator is the successful seated participants that return a complete,
valid ballot in that discussion cycle. Abstentions remain in that denominator
but do not by themselves count as disagreement or extend discussion. The saved
`deadline_ms` is the overall analysis-plus-discussion budget; an omitted or zero
legacy value is normalized to 360 seconds.

After deterministic synthesis, a preset may optionally require a **chairman**.
The chairman is one configured, enabled review agent selected visibly in the
Roundtable GUI. It receives the original request, reviewed artifact, independent
reports, and deterministic feedback, then submits one final structured verdict.
There is no chairman retry across the roster: an unavailable chairman, malformed
verdict, stage mismatch, or missing original-request alignment parks the workflow
visibly. With the chairman disabled, deterministic synthesis is final.

Every review must explicitly report `original_request_alignment` as `aligned`,
`drifted`, or `unclear`, with a comparison to the originating request. A useful
refinement remains aligned; substituting an unrelated goal or deliverable is
drift. WFE gates fail closed on `drifted`, `unclear`, or an omitted assessment,
so implementation quality cannot accidentally earn approval for work that does
not answer the request.

Roundtable participation metadata follows three rules:

- `participants_failed` counts unusable responses in the adopted best round;
  `participants_required_failed` counts the subset in its caller-authored,
  required prefix. The latter is additive response metadata.
- Every configured seat is attempted and remains visible in the total. There are
  no automatically filled capacity seats.
- `required_participants == 0` preserves the legacy all-required behavior. A
  value larger than the effective panel fails closed before invoking a model.

`degraded` describes the run as a whole and is intentionally sticky: truncation,
a failed required seat, insufficient successful responses,
an aggregator failure, or verification degradation cannot be hidden merely by
selecting a later artifact. Consequently, an adopted round with zero required
failures does not clear an earlier run-level degradation.

The execution deadline is tuned by `roundtable.deadline_ms`. The workflow engine's
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
