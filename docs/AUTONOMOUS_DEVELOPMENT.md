# Autonomous Development

> **Hand aimee a proposal; it builds the change end-to-end.** You submit a
> written proposal and aimee runs the *entire* development lifecycle (design,
> plan, implement, review, PR) **server-side and unattended**. Closing
> the browser tab does not stop it. This is **core functionality and is on by
> default**; safety comes from the workflow's *gates*, not an off-switch.
>
> aimee **never starts work on its own.** Every autonomous run begins from a
> human-submitted proposal. There is no "find work to do" loop.

## What it is

Autonomous development turns a proposal into a finished, reviewed pull request
without a human driving each step. It is a thin orchestration layer over three
things aimee already has:

- the **workflow engine** (`wfe`), a block-composed state machine with durable
  per-work-item state, gates, and an audit log;
- **delegates**, sub-agents that do bounded work (here: write code with tools in
  an isolated worktree); and
- the **server-owned turn lifecycle**, a turn/run is owned by the server, not by
  a client connection, so it runs to completion regardless of who is watching.

The **primary agent manages; delegates do the work.** The primary (the engine
plus a coordinator) decomposes the plan, dispatches each unit to a delegate,
verifies the result, and, if the work is not acceptable, sends it back to a
*different* delegate. The primary never writes the code itself.

Throughout this page, a **run** and a **work item** are the same thing: one
execution of a workflow for one proposal. The **primary agent** is the manager
(the engine + coordinator); **delegates** are the workers.

## Prerequisites

- A running aimee server (`aimee-server`, or the combined image). The feature is
  default-on, there is nothing to enable.
- An API token for the `/v1` surface (`Authorization: Bearer …`). On a remote
  client this is your configured server token; locally the dev bearer works.
- The default `build` workflow is **seeded automatically** into
  `$AIMEE_HOME/workflows` at standup, so a fresh server resolves it out of the
  box. Custom workflows live in the same directory.
- Delegates **ship configured** (a default roster + roundtable panel). See
  [Delegates](DELEGATES.md) only to add your own providers; verify with
  `aimee --json agent list`.
- For the final merge step, the server needs forge access (a configured `gh` /
  git credential) and a `testing` branch; autonomous merges target `testing`
  only.

## Quick start

Submit a proposal for autonomous execution:

```bash
curl -k -sX POST https://127.0.0.1:8743/v1/dev/submit \
  -H "Authorization: Bearer $AIMEE_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"proposal_md": "## Add a /healthz route returning {status:ok}\n\nWhy: ...\nAcceptance: GET /healthz returns 200 with {\"status\":\"ok\"}."}'
# -> {"work_item_id":"wi-...","workflow":"build","state":"active"}
```

A `401` means the token is missing/invalid; `400` means `proposal_md` was empty.
On success the run proceeds on the server, you can close the connection and it
continues. The webchat **Workflows** tab is the GUI for all of this: a **Submit
proposal** panel (textarea → runs on the chosen workflow), a live **run list**
with each run's stage/state, and **Approve / Reject** buttons on a run parked at
a human gate (operator-only). You can also drive it purely via the API as above.

Request fields:

| field | required | meaning |
|-------|----------|---------|
| `proposal_md` | yes | the proposal to execute (Markdown) |
| `workflow` | no | workflow name; defaults to `build` |
| `repo` | no | repository identifier for the run |

## The lifecycle

A run is a work item bound to a **workflow**, by default `build`
(`config/workflows/build.yaml`), the standard dev lifecycle:

```
author.proposal → gate.roundtable → pr.open → gate.human (approve) →
author.plan    → gate.roundtable →
implement      → freeze → gate.roundtable → pr.open → gate.human (pass/fail)
                                                         pass → merge
                                                         fail → implement (loop)
```

`freeze` captures the cumulative diff at a stable commit before review, so the
roundtable and the final PR see one coherent change rather than a moving target.
`gate.roundtable` is a machine gate (a model panel verdict); `gate.human` is a
human gate; `gate.ci` polls the PR's CI. The `fail → implement` arrow is the
re-delegate loop.

Workflows are user-editable; clone and change the composition with
`aimee workflow new`. The engine has no privilege over a user-authored workflow.
A custom workflow is just another YAML in `$AIMEE_HOME/workflows`; submit against
it by passing its `workflow` name. For example, inserting a `gate.roundtable`
with a `security` lens before `pr.open` adds a mandatory security review.

### The implement stage: manage → delegate → verify → re-delegate

`implement` is where "the primary manages, delegates do the work" is concrete:

1. **Split.** The plan is decomposed into independent, file/module-scoped units.
2. **Dispatch.** Each unit goes to an `engineer` delegate that runs **with tools
   pinned to the work item's own git worktree**, it edits files in place, and
   the change is committed on the work-item branch.
3. **Verify, as hard as possible.** Verification runs through the engine's gates
   and delegates, never the primary's own hands, in ascending cost:
   - mechanical, the build compiles, targeted tests/lint pass (`gate.ci`);
   - review, a roundtable / `reviewer` panel checks the change against its spec
     (`gate.roundtable`);
   - adversarial, for risky changes, skeptic verifiers attempt to refute it.
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
- **never forges a human approval**, gate-override is a signed, human-only action
  capped at a small number of uses.

Approve or reject a parked gate from the webchat **Workflow Actions** page's detail view
panel, or via `POST /v1/workflow/items/<id>/gate {decision}`. The endpoint is
**operator-gated** (`CAP_WORKFLOW_ADMIN`, outside the ordinary authenticated
capability set), and approvals are HMAC-signed server-side with the operator key
(`$AIMEE_HOME/.approval-key`) and content-hash-bound, so a delegate can never
forge one. `reject` is terminal; `approve` resumes the run.

## Server-owned execution

The run and its delegate turns are owned by the server, not your connection:

- a turn publishes its full stream to the presence event ring and runs to
  completion even if the client drops;
- the **autonomy scheduler**, a background thread, drives every active
  autonomous work item forward, waking on a new submission, on a satisfied gate,
  and on a periodic backstop sweep (crash recovery);
- reconnecting replays the run from a cursor, so you see what happened while you
  were away.

Interactive (human-driven) work items are left alone by the scheduler.

## Safety and limits

Autonomous development is default-on; the guardrails are structural, not a toggle:

- **Human gates** are never auto-satisfied unless preauthorized for that run, and
  gate-override stays human-only and capped.
- **Per-run budget ceiling.** A work item carries a cost cap; on breach the run
  **parks** (`budget_exceeded`) for a human, it never silently runs away.
  (Today the cap is a work-item field; setting it *from* `/v1/dev/submit` is not
  yet exposed, see [Current limitations](#current-limitations).)
- **Autonomous merges only target `testing`.** Promotion to `main` is always a
  human action.
- **Every commit and PR is audited** and attributed to the run. Generated commits
  and PRs carry **no AI co-authorship trailers** (enforced repo-wide by a
  required CI check).
- **Never auto-retry without new input** (a CI log, a roundtable verdict), this
  prevents infinite loops.
- A delegate's model refusal or a permanent/unrecoverable error terminates the
  unit; transient errors are retried within the cap; degraded panels, budget
  breaches, and forge failures park.

### Tuning (web Settings → `autonomy.*`)

The pipeline knobs are typed config fields, editable in the web **⚙️ Settings** page
(under `autonomy.`) or `aimee.yaml`; a change applies on the next server start (an
exported `AIMEE_AUTONOMY_*` env var still overrides). See [SETTINGS.md](SETTINGS.md).

| Knob | Default | Effect |
| --- | --- | --- |
| `autonomy.skeptics` | 0 (off) | N adversarial skeptics on the implement gate. |
| `autonomy.fanout` | off | Engine-level fan-out manager loop vs a single implement dispatch. |
| `autonomy.unit_retry` | 2 | Per-unit retry-different-delegate cap under fan-out. |
| `autonomy.unit_max` | 16 | Max fan-out units (a larger decomposition parks). |
| `autonomy.ci_retry_max` | 2 | Per-work-item red-CI retry cap before parking. |

## Observability

- **Webchat Workflow Actions page**, live progress per work item: current stage, gate
  verdicts, parked state, and the actionable approve/reject controls for human
  gates.
- **Event stream**, a run publishes its full turn stream (text, tool calls,
  usage, boundaries) to the presence event ring; the webchat replays it live and,
  after a disconnect, from a cursor.
- **Durable audit**, every step, verdict, and cost is recorded in the DB1
  `lifecycle_*` tables (per work item), including the commit hashes produced and
  the gate decisions. This is the source of truth for "what did the run do".
- **Attribution**, autonomous commits and PRs are attributed to the run and
  carry **no** AI co-authorship trailers.

## When a run parks (failure modes & recovery)

The run **parks** instead of failing hard when a human is needed; the scheduler
resumes it automatically once the blocker clears:

| pause reason | what it means | how it resumes |
|--------------|---------------|----------------|
| `pending_human` | waiting at a human gate (proposal approval / final pass-fail) | approve or reject in the webchat Workflow Actions page → the scheduler re-drives |
| `panel_degraded` / `panel_unreachable` | the roundtable couldn't reach a quorum | retried on the next sweep; a human can approve to proceed |
| `ci_pending` | the PR's CI hasn't concluded | re-checked on the next sweep |
| `merge_pending` | the forge merge state is undeterminable | re-checked on the next sweep |
| `budget_exceeded` | the run hit its cost cap | a human decides whether to continue |

The autonomy driver **never forges a human approval**. A stuck human gate can be
cleared by a signed, human-only **gate-override** (capped at a small number of
uses); after that it forces a terminal `rejected`. Machine failures follow the
taxonomy in [Safety and limits](#safety-and-limits): transient → retry,
permanent/refusal → terminal, degraded/budget/forge → park, and a unit is never
auto-retried without new input (a CI log or a roundtable verdict).

## Current limitations

Honest scope of the current implementation (the design allows for more):

- **Submit takes `proposal_md`, `workflow`, `repo` only.** Per-run `limits` and
  gate **preauthorization** are not yet accepted at `/v1/dev/submit`; gates are
  handled interactively via the webchat, and the cost cap is a work-item field.
- **No dedicated `resume`/`abort`/`audit` HTTP endpoints yet.** Resume is
  event-driven (gate approval + the scheduler sweep); drive and inspect runs via
  the webchat Workflow Actions page and the `lifecycle_*` audit tables.
- **Scheduler concurrency is 1** (runs are driven sequentially) for now; the
  design's bounded-concurrency fan-out is a follow-on.
- **`build.yaml` must be present** in `$AIMEE_HOME/workflows` (seeded at standup);
  a submission against a missing workflow returns an error.

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
| `POST /v1/dev/submit` (`rh_dev_submit`) | intake, the only entry; creates an autonomous work item, seeds the proposal, notifies the scheduler |
| autonomy scheduler (`wfe_scheduler`) | server-owned thread driving active autonomous work items via `wfe_autonomy_run` |
| workflow engine (`wfe_*`) | block-composed lifecycle, gates, durable state + audit (`lifecycle_*` tables) |
| live delegate provider (`wfe_live_delegate`) | the engine→delegate bridge: runs a block's role as a tool-using agent in the work-item worktree, then commits. (It uses the same in-process delegate execution as `aimee delegate`/`/v1/delegate/run`, but is invoked *by the engine* per block rather than by a user.) |
| autonomy driver (`wfe_autonomy`) | advances machine gates, parks at human gates, never forges approval |
| turn registry / server-owned turns | a run survives client disconnect (the foundation) |

**Invariants**: aimee never self-initiates; the primary manages
while delegates do the work; rejected work is re-delegated; **all** autonomous
action flows through the workflow engine, there is no side-channel that takes an
autonomous action outside the engine's gates, budget, and audit.

## See also

- [Delegates](DELEGATES.md), the sub-agents that do the work
- [Architecture](ARCHITECTURE.md), where the engine sits in the system
- `docs/proposals/pending/full-autonomous-development.md`, the design + the
  roundtable-resolved open questions
