# Workflows

A **workflow** is a declarative graph that encodes a development lifecycle,
*propose → review → approve → plan → implement → freeze → review → PR → merge*,
as composable, typed steps. aimee ships one default composition (`build`), and
you can clone and edit it or author your own. The engine that advances a
workflow run is the same one that drives delegates, roundtable reviews, and
human approval gates.

> **Status (current):** authoring and validating workflows is fully supported
> (CLI + the webchat **Edit Workflows** tab); starting a run and watching its
> status/history live on the **Workflow Actions** tab
> ([`WORKFLOW_ACTIONS.md`](WORKFLOW_ACTIONS.md)). The execution engine,
> stepping a run through its gates, roundtables, and approvals, is implemented,
> tested, and reachable: `POST /v1/dev/submit` seeds a proposal, creates an
> autonomous work item on the chosen workflow (default `build`), and the
> scheduler drives it server-side. See
> [Autonomous Development](AUTONOMOUS_DEVELOPMENT.md). The chosen `workflow` is
> authoritative, so **any** saved workflow starts this way, not only `build`.
> You can start a run from the CLI with `aimee workflow run <name> --proposal
> <file>` (or `--message`, or `-` for stdin) and watch it with `aimee workflow
> status <id> --watch`; agents can start one over MCP with the `workflow_run`
> tool. Both go through the same capped/audited intake as `POST /v1/dev/submit`,
> so a proposal is still required (the run is framed as *propose → … → PR*). See
> [Limitations](#current-limitations).

## Mental model

A workflow definition (`wfe_def_t`, [src/workflow/wfe_def.h](../src/workflow/wfe_def.h))
is a named graph:

- **nodes**, each node is one **step**: an `id`, a **block** (the step's type),
  optional `params`, and typed inputs (`in`).
- **blocks**, a block is the *kind* of work a step does (write a proposal, run a
  roundtable, open a PR, …). The catalog is fixed in code; you can also define
  **custom blocks**. See [Block catalog](#block-catalog).
- **control edges**, how the run moves between steps:
  - `next`, unconditional successor.
  - `on_pass` / `on_fail`, a **gate**'s two outcomes (a gate produces a verdict
    or approval; pass takes one edge, fail the other).
- **data edges** (`in`), bind a step's typed input slot to an upstream step's
  output (`<producer_id>.<output>`, default output handle `out`). These are
  *type-checked*: e.g. `implement` only accepts a `plan`, `merge` only a `pr`.
- **start**, the entry node id (defaults to the first node).

Every block declares the **artifact type** it produces and the types it accepts,
so the validator rejects a graph that wires a proposal into a step expecting a
PR. The pipeline of artifact types is:

```
proposal → plan → branch → frozen_diff → pr → (merged)
                     │
   roundtable/CI/mergeable gates emit a `verdict`
   human gates emit an `approval`
```

## Block catalog

From the catalog in [src/workflow/wfe_def.c](../src/workflow/wfe_def.c)
(`aimee workflow blocks` prints the live list):

| Block | Produces | Accepts (input) | Kind | What it does |
|---|---|---|---|---|
| `author.proposal` | proposal |, (no input) | action | A delegate drafts a proposal. The usual entry point. |
| `author.plan` | plan | proposal | action | A delegate turns the proposal into an implementation plan. |
| `implement` | branch | plan | action | Delegates implement the plan onto a branch (`params.fanout: max` parallelizes). |
| `document` | branch | branch | action | A delegate writes docs onto the branch. Composes between implement and freeze. |
| `freeze` | frozen_diff | branch | action | Freezes the branch to an immutable diff for review. |
| `gate.roundtable` | verdict | proposal · plan · frozen_diff | **gate** | Runs a multi-persona review **panel**; pass/fail on quorum. |
| `gate.human` | approval | proposal · plan · branch · frozen_diff · pr | **gate** | Parks for a human decision — **inviolable**; never auto-satisfied in autonomous mode. |
| `pr.open` | pr | proposal · frozen_diff | action | Opens a pull request. |
| `merge` |, (terminal) | pr | action | Merges the PR. |
| `gate.ci` | verdict | pr | **gate** | Polls the PR's CI; **fail-closed** (no green → fail). |
| `check.mergeable` | verdict | pr | **gate** | Refuses on a merge conflict. |
| `custom` | declared | declared | either | A config-defined block (command or delegate), see [Custom blocks](#custom-blocks). |

**Gates** are the blocks that branch: they take `on_pass`/`on_fail` instead of a
single `next`. Everything else takes `next`.

## The default `build` workflow

[config/workflows/build.yaml](../config/workflows/build.yaml) is the reference
composition, the full two-gate development lifecycle. The control flow:

```
draft (author.proposal, with_user)
  └─next→ proposal_gate (roundtable: security, architect, qa, reviewer; quorum 4)
            ├─pass→ proposal_pr (pr.open) ─next→ proposal_approve (human: pr_review)
            │                                        └─next→ plan (author.plan)
            └─fail→ draft                                       └─next→ plan_gate (roundtable)
                                                                          ├─pass→ impl (implement, fanout max)
                                                                          └─fail→ plan
impl ─next→ document ─next→ freeze ─next→ impl_gate (roundtable)
                                            ├─pass→ code_pr (pr.open) ─next→ pr_passfail (human)
                                            └─fail→ impl                        ├─pass→ check_mergeable
                                                                                └─fail→ impl
check_mergeable ─pass→ gate_ci ─pass→ merge        (any gate fail → back to impl)
```

A roundtable node carries its panel in `params`:

```yaml
- id: proposal_gate
  block: gate.roundtable
  in:
    src: draft.out          # bind this gate's input to draft's output
  params:
    panel:
      required: [security, architect, qa, reviewer]   # persona names
      eligible: [contrarian]                           # may join if available
    quorum: 4
    max_rounds: 6
  on_pass: proposal_pr
  on_fail: draft
```

Panel entries are **persona** names, see [Personas](personas.md). Each panelist
is a persona run on a delegate model; the gate passes when the quorum of
panelists approve (or fails after `max_rounds`). Panelists are dispatched **in
parallel**, so a round costs roughly one model's latency rather than the sum.

**Which model reviews each persona** comes from the active roundtable preset (the
one the GUI edits and `roundtable.default` selects). For each required persona the
gate looks up the matching seat and honors its model:

- a **specific pinned model** is dispatched to that **exact** agent. If that model
  is not enabled/routable for the `review` role — or its dispatch fails — the run
  **fails** (a pinned model is a hard requirement, never silently swapped);
- a **`$random`** seat (or a persona with no matching seat) picks **any**
  review-capable agent and retries a different one until one is accepted, so a
  flaky agent doesn't stall the gate.

Set a seat to a specific model or to *Random* in the Roundtable page of the GUI.
An agent is "review-capable" unless its `exec_roles` explicitly omit `review`
(e.g. a specialized `gpu-mid` is never seated). If no panel of reviewers can be
composed at all the gate parks `panel_degraded` for a human.

A gate convenes the configured default roundtable (`roundtable.default`) unless
the node names a specific preset with `params.roundtable` — so different gates in
one workflow can use different panels:

```yaml
- id: plan_gate
  block: gate.roundtable
  params:
    roundtable: security-review     # convene this preset's seats (else the default)
    panel:
      required: [security, architect, qa, reviewer]
    quorum: 4
```

If the named preset does not exist the gate logs a warning and every lens falls
back to `$random`.

There is **no engine privilege** over a user-authored workflow: `build` is just
one composition of the same catalog you compose from. Clone it and edit freely.

## Authoring

### Definition format

A workflow is YAML: a `name`, a `start` node id, and a `nodes` list. Each node:

```yaml
name: my-workflow
start: draft
nodes:
  - id: draft
    block: author.proposal
    params: { with_user: true }
    next: review

  - id: review
    block: gate.roundtable
    in: { src: draft.out }          # input slot ← producer.output
    params:
      panel: { required: [reviewer, qa] }
      quorum: 2
    on_pass: pr
    on_fail: draft                  # loop back to revise

  - id: pr
    block: pr.open
    in: { src: draft.out }
```

Definitions are **content-addressed**: the engine computes a canonical form and a
SHA-256 `version` over it ([wfe_canonical.c](../src/workflow/wfe_canonical.c)), so
a run is pinned to the exact graph it started on.

### CLI

```
aimee workflow blocks               # list the composable block catalog
aimee workflow new <file.yaml>      # scaffold a starter workflow
aimee workflow validate <file.yaml> # typed-graph validation (does it wire up?)
aimee workflow show <file.yaml>     # print the canonical form + version
aimee workflow list                 # list workflows under $AIMEE_HOME/workflows
```

Workflow files live under `$AIMEE_HOME/workflows/`. `validate` runs the same
type-checker the engine uses: it catches dangling edges, a step fed the wrong
artifact type, or a missing required input.

### Webchat: Edit Workflows tab

The browser **Edit Workflows** tab (route `/edit-workflows`) is a visual composer
over the same definitions. Authoring a run, watching its status/history, and
deciding human gates live on the separate **Workflow Actions** tab
(`/workflow-actions`, see [`WORKFLOW_ACTIONS.md`](WORKFLOW_ACTIONS.md)):

- A **blocks rail** to add steps, a **canvas** that lays the graph out
  left-to-right by depth with colored edges (solid = `next`, green = `on pass`,
  red = `on fail`, faint = data dependency), and an **inspector** to set a step's
  title, block, params, per-step **persona/delegate** assignment, and its
  `next`/`on_pass`/`on_fail` edges.
- **Personas** can be created and edited from the same tab (it proxies
  `/api/chat/personas`).
- **Validate** and **Save** persist the def server-side via `/api/workflow/*`.
- Each top-nav tab (including Edit Workflows) selects its own git **project**.

> Note: adding a step from the blocks rail drops it onto the canvas
> **disconnected**, you wire it into the sequence by selecting it and setting
> its `next`/`on_pass`/`on_fail` in the inspector. To start and watch a run, use the Workflow Actions tab.

### Custom blocks

Beyond the built-in catalog you can declare custom blocks in
`$AIMEE_HOME/workflows/blocks.yaml`
([wfe_custom.c](../src/workflow/wfe_custom.c)). A custom block has a name, a typed
I/O signature (so it type-checks like a built-in), and an executor that is either
a **command** (`WFE_EXEC_COMMAND`, an argv) or a **delegate**
(`WFE_EXEC_DELEGATE`). The validator and artifact-type system consult the
registry, so custom and built-in blocks compose identically.

## Execution model

When a run exists, it is a **work-item** (the `lifecycle_work_item` table,
[src/db1/wfe_store.c](../src/db1/wfe_store.c)) carrying: the workflow name and
pinned `version`, the target `repo`, `current_stage`, `content_hash`, `state`,
`mode` (`interactive` | `autonomous`), `pause_reason`, and accumulated cost.

The engine advances it one step at a time
([wfe_engine.c](../src/workflow/wfe_engine.c) `wfe_engine_advance`):

- An **action** block runs (often dispatching a delegate with a persona) and
  advances along `next`.
- A **gate** evaluates and takes `on_pass` or `on_fail`.
- A **roundtable** gate runs its persona panel as delegates and passes on quorum.
- A **human gate** **parks** the run (`WFE_STEP_PENDING`) until a human approval is
  recorded — and this is **inviolable**. In `autonomous` mode the driver
  ([wfe_autonomy.c](../src/workflow/wfe_autonomy.c)) **never** auto-satisfies a
  human gate; authoring one as auto-satisfiable (`policy: preauthorized` or
  `optional: true`) is rejected at validation. Only a human's signed approval
  clears it.
- Approvals are **signed** ([wfe_approval.c](../src/workflow/wfe_approval.c)) and
  recorded against the step's content hash, so an approval is bound to the exact
  artifact it approved.

A step resolves the working repository from the work item's own `repo` when it
names a local directory (a trigger rule's `pipeline.workspace` binds the run to
that repository), falling back to `$AIMEE_WORKFLOW_REPO`, then the process cwd.
See [Limitations](#current-limitations) for the forge-side residue of the old
process-global behavior.

## Inspecting runs

When work-items exist, they are readable (the Workflow Actions tab (see [`WORKFLOW_ACTIONS.md`](WORKFLOW_ACTIONS.md)) and
the API):

- CLI: `aimee cancel <work-item-id>` cancels a run.
- Webchat: `GET /api/workflow/items` (and `/items/<id>`) list run state; a run
  bound to a chat channel can be paused via
  `POST /api/sessions/workflows/<id>/pause`.
- Server `/v1/workflow/*` ([server_workflow_api.c](../src/server/server_workflow_api.c))
  exposes the def read/author surface and the work-item read surface.

## Triggers

A **trigger** is the *first step* that starts a workflow run — the thing that
decides *when* a run begins and *which* workflow it begins. A trigger is
deliberately generic: the event that fires it can be autonomous (a new proposal
appears in a repo, a schedule elapses, a webhook arrives) or a person (clicking
**Run** on the Workflow Actions tab is itself a trigger). The intent is a small
set of generic trigger sources that you wire to whatever workflow you want —
the trigger starts the run; what happens next is entirely the workflow's design.

Sources are a registry in
[src/server/trigger_scheduler.c](../src/server/trigger_scheduler.c): each entry
is `{name, due, fire}` — `due` decides whether this tick should fire (a repo
scanner polls every pass; cron matches its schedule), `fire` matches events and
materializes artifacts, and files each run through the shared
`trigger_file_run` back half (work item on the rule's pipeline + workspace +
mode, per-run USD ceiling from `max_spend_usd` or the intake-wide default,
audit log line). The tick loop owns the rest — per-rule rate limiting and the
global `trigger.max_concurrent` — so adding a source is one table row plus its
`fire`.

Triggers are configured as a `trigger_rules` list in `aimee.yaml`. Each rule
names a **source** (what fires it), how the filed run should execute (**mode**),
and the **pipeline** (which workflow, in which repo):

```yaml
trigger:
  max_concurrent: 1          # cap on concurrently-executing triggered runs

trigger_rules:
  - source: proposals              # scan a git repo for new proposals
    event: docs/proposals/pending  # repo-relative dir to watch (this is the default)
    schedule: main                 # git ref/branch to read (default: auto-detected origin HEAD)
    mode: autonomous               # autonomous (default) | interactive
    pipeline:
      template: build              # the workflow to start for each new proposal
      workspace: /srv/repos/myproject   # absolute path of the git repo to scan
```

### Rule fields

| Field | Meaning |
| --- | --- |
| `source` | What fires the rule: `proposals`, `cron`, or a webhook source. |
| `event` | Source-specific match. For `proposals`, the repo-relative directory to scan (default `docs/proposals/pending`). |
| `schedule` | For `cron`, the cron expression. For `proposals`, the git ref/branch to read (default: auto-detected `origin` HEAD, then `HEAD`). |
| `mode` | Execution mode stamped on the filed work item — see below. `autonomous` (default) or `interactive`. |
| `pipeline.template` | The workflow to start (any saved workflow name, e.g. `build`). |
| `pipeline.workspace` | Absolute path of the git repo the run operates in (and, for `proposals`, the repo to scan). |
| `pipeline.max_spend_usd` | Optional per-run spend cap. |

### `mode`: autonomous vs. interactive

`mode` selects **how the run executes once the trigger files it**, independent
of the workflow it names:

- **`autonomous`** (the default) — the autonomy scheduler drives the run
  hands-off, start to finish. Use this when the trigger event *is* the approval:
  merging a proposal into the watched branch, for example, is itself the decision
  to build it, so no further human sign-off is wanted.
- **`interactive`** — the run is filed and then **parks for a human** to drive in
  the webchat (Workflow Actions tab). Use this when someone should review before
  anything runs.

This is distinct from **gating inside the workflow**. A `gate.human` step (or an
`author.proposal` node marked `with_user`) parks *that step* for a person
regardless of `mode` — it is a property of the workflow you build, not of the
trigger. So you can compose either shape: an `autonomous` trigger into a
workflow that still pauses at a human gate partway through, or an `interactive`
trigger into a fully hands-off workflow. Pick the trigger `mode` for the
*start* decision; put in-flight gates in the *workflow*.

### The `proposals` source

The `proposals` source turns "a proposal was committed" into a workflow run. On
each scan (roughly once a minute) it lists the proposal directory at the
configured git ref, and for every proposal it has not seen before it:

1. materializes the proposal's content under `$AIMEE_HOME/triggers/proposals/`,
2. files exactly one work item on `pipeline.template`, in `pipeline.workspace`,
   with the configured `mode` (default `autonomous`),
3. de-duplicates by proposal, so re-scanning the same tree never files twice,
4. stamps the run's USD ceiling — the rule's `max_spend_usd` if set, else the
   same default every autonomous intake gets ($5.00; `AIMEE_AUTONOMY_MAX_USD`
   overrides, `0` disables) — and wakes the autonomy scheduler so the run
   starts immediately rather than on its next backstop sweep.

`trigger.max_concurrent` is enforced at filing time: when that many
trigger-filed runs are still active, further proposals are deferred (not
dropped — the dedup key is the materialized proposal, so they file on a later
scan once a slot frees). The work item's `repo` is the rule's
`pipeline.workspace`, and every per-step block resolves its working directory
from it, so the run executes in the watched repository.

Merging a new proposal onto the watched branch is therefore all it takes to
kick off a build — and with `mode: autonomous`, that build runs to its PR
without further intervention. Point the rule at a different `template` to run
any other workflow you have authored.

> The filed run still enters through the same capped, audited intake as every
> other run (see [Autonomous Development](AUTONOMOUS_DEVELOPMENT.md)); the trigger
> supplies the proposal text, so the *propose → … → PR* framing below still holds.

## Current limitations

These are real today and worth knowing before you lean on workflows:

1. **Every run is still framed as a proposal.** Runs start through the capped,
   audited intake behind `POST /v1/dev/submit` (see
   [Autonomous Development](AUTONOMOUS_DEVELOPMENT.md)). The `workflow` field is
   authoritative, so **any** saved workflow starts this way, not just `build`.
   That intake is now reachable three ways — the **Workflow Actions** tab, the
   `aimee workflow run <name>` CLI command, and the `workflow_run` MCP tool for
   agents — but all of them require a `proposal_md`: there is still no way to
   start a workflow whose entry node isn't `author.proposal` without supplying
   proposal text. (There is still no Run button on the **Edit Workflows** page.)
2. **Forge operations are process-global.** Per-step blocks now resolve their
   working directory from the work-item's `repo` (when it names a local
   directory; `$AIMEE_WORKFLOW_REPO`/cwd otherwise), so a triggered run executes
   in its configured workspace. But the forge half (`git push`, `gh pr …` via
   the vaulted runner) still runs in the server's own checkout, so PR-opening
   workflows against a workspace that is not the server's primary repo are not
   yet fully wired.
3. **Composer ergonomics.** New steps are added disconnected; you wire order in
   the inspector. The **Edit Workflows** page has no live run/progress view — you
   start and watch runs on the separate **Workflow Actions** tab.

## See also

- [Personas](personas.md), the identities that staff roundtable panels and steps.
- [Delegates](DELEGATES.md), how steps dispatch model work.
- [Architecture](ARCHITECTURE.md), where the workflow engine sits.
