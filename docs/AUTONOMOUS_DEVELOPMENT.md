# Autonomous Development

> **Hand aimee a proposal; it builds the change end-to-end.** You submit a
> written proposal and aimee runs the *entire* development lifecycle —
> design, plan, implement, review, PR — **server-side and unattended**. Closing
> the browser tab does not stop it. This is **core functionality and is on by
> default**; safety comes from the workflow's *gates*, not an off-switch.
>
> aimee **never starts work on its own.** Every autonomous run begins from a
> human-submitted proposal. There is no "find work to do" loop.

## What it is

Autonomous development turns a proposal into a finished, reviewed pull request
without a human driving each step. It is a thin orchestration layer over three
things aimee already has:

- the **workflow engine** (`wfe`) — a block-composed state machine with durable
  per-work-item state, gates, and an audit log;
- **delegates** — sub-agents that do bounded work (here: write code with tools in
  an isolated worktree); and
- the **server-owned turn lifecycle** — a turn/run is owned by the server, not by
  a client connection, so it runs to completion regardless of who is watching.

The **primary agent manages; delegates do the work.** The primary (the engine
plus a coordinator) decomposes the plan, dispatches each unit to a delegate,
verifies the result, and — if the work is not acceptable — sends it back to a
*different* delegate. The primary never writes the code itself.

## Quick start

Submit a proposal for autonomous execution:

```bash
curl -sX POST http://127.0.0.1:8740/v1/dev/submit \
  -H "Authorization: Bearer $AIMEE_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"proposal_md": "## Add a /healthz route returning {status:ok}\n\nWhy: ...\nAcceptance: GET /healthz returns 200 with {\"status\":\"ok\"}."}'
# -> {"work_item_id":"wi-...","workflow":"build","state":"active"}
```

The run now proceeds on the server. You can close the connection; it continues.
The webchat **Workflows** tab shows live progress and surfaces the human gates as
actionable approvals.

Request fields:

| field | required | meaning |
|-------|----------|---------|
| `proposal_md` | yes | the proposal to execute (Markdown) |
| `workflow` | no | workflow name; defaults to `build` |
| `repo` | no | repository identifier for the run |

## The lifecycle

A run is a work item bound to a **workflow** — by default `build`
(`config/workflows/build.yaml`), the standard dev lifecycle:

```
author.proposal → gate.roundtable → pr.open → gate.human (approve) →
author.plan    → gate.roundtable →
implement      → freeze → gate.roundtable → pr.open → gate.human (pass/fail)
                                                         pass → merge
                                                         fail → implement (loop)
```

Workflows are user-editable; clone and change the composition with
`aimee workflow new`. The engine has no privilege over a user-authored workflow.

### The implement stage: manage → delegate → verify → re-delegate

`implement` is where "the primary manages, delegates do the work" is concrete:

1. **Split.** The plan is decomposed into independent, file/module-scoped units.
2. **Dispatch.** Each unit goes to an `engineer` delegate that runs **with tools
   pinned to the work item's own git worktree** — it edits files in place — and
   the change is committed on the work-item branch.
3. **Verify, as hard as possible.** Verification runs through the engine's gates
   and delegates, never the primary's own hands, in ascending cost:
   - mechanical — the build compiles, targeted tests/lint pass (`gate.ci`);
   - review — a roundtable / `reviewer` panel checks the change against its spec
     (`gate.roundtable`);
   - adversarial — for risky changes, skeptic verifiers attempt to refute it.
4. **Re-delegate on reject.** When verification rejects a unit, the engine loops
   back to `implement`, which re-dispatches the unit to a *different* delegate
   (fresh perspective), bounded by a retry cap; on exhaustion it parks for a
   human.

Only verified work advances.

## Human gates

Autonomous does not mean unsupervised. The workflow reserves **human gates**
(proposal approval, final pass/fail). In autonomous mode the driver:

- auto-advances *machine* gates (CI, mergeability, roundtable verdicts);
- auto-satisfies **only** the human gates the submitter explicitly preauthorized;
- **parks** at every other human gate (`pending_human`) until a person acts;
- **never forges a human approval** — gate-override is a signed, human-only action
  capped at a small number of uses.

Approve or reject a parked gate from the webchat Workflows tab (or the gate API).

## Server-owned execution

The run and its delegate turns are owned by the server, not your connection:

- a turn publishes its full stream to the presence event ring and runs to
  completion even if the client drops;
- the **autonomy scheduler** — a background thread — drives every active
  autonomous work item forward, waking on a new submission, on a satisfied gate,
  and on a periodic backstop sweep (crash recovery);
- reconnecting replays the run from a cursor, so you see what happened while you
  were away.

Interactive (human-driven) work items are left alone by the scheduler.

## Safety and limits

Autonomous development is default-on; the guardrails are structural, not a toggle:

- **Human gates** are never auto-satisfied unless preauthorized for that run, and
  gate-override stays human-only and capped.
- **Per-run budget ceilings** (turns / tokens / wall-clock). On breach the run
  **parks** for a human — it never silently runs away. Set a cap per submission.
- **Autonomous merges only target `testing`.** Promotion to `main` is always a
  human action.
- **Every commit and PR is audited** and attributed to the run. Generated commits
  and PRs carry **no AI co-authorship trailers** (enforced repo-wide by a
  required CI check).
- **Never auto-retry without new input** (a CI log, a roundtable verdict) — this
  prevents infinite loops.
- A delegate's model refusal or a permanent/unrecoverable error terminates the
  unit; transient errors are retried within the cap; degraded panels, budget
  breaches, and forge failures park.

## Configuration

- **Default workflow.** `build` is seeded into `$AIMEE_HOME/workflows` at standup
  (baked into the server/combined images). Custom workflows live alongside it.
- **Choosing a workflow.** Pass `workflow` to `/v1/dev/submit`, or author your
  own with `aimee workflow new` and submit against it.
- **Delegate roster.** The shipped roster + roundtable panel are used as-is; see
  [Delegates](DELEGATES.md) to add providers or change the panel.

## How it fits together (internals)

| component | role |
|-----------|------|
| `POST /v1/dev/submit` (`rh_dev_submit`) | intake — the only entry; creates an autonomous work item, seeds the proposal, notifies the scheduler |
| autonomy scheduler (`wfe_scheduler`) | server-owned thread driving active autonomous work items via `wfe_autonomy_run` |
| workflow engine (`wfe_*`) | block-composed lifecycle, gates, durable state + audit (`lifecycle_*` tables) |
| live delegate provider (`wfe_live_delegate`) | runs a block's role as a tool-using delegate in the work-item worktree, then commits |
| autonomy driver (`wfe_autonomy`) | advances machine gates, parks at human gates, never forges approval |
| turn registry / server-owned turns | a run survives client disconnect (the foundation) |

**Invariants** (by construction): aimee never self-initiates; the primary manages
while delegates do the work; rejected work is re-delegated; **all** autonomous
action flows through the workflow engine — there is no side-channel that takes an
autonomous action outside the engine's gates, budget, and audit.

## See also

- [Delegates](DELEGATES.md) — the sub-agents that do the work
- [Architecture](ARCHITECTURE.md) — where the engine sits in the system
- `docs/proposals/pending/full-autonomous-development.md` — the design + the
  roundtable-resolved open questions
