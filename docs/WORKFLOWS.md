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
> [Autonomous Development](AUTONOMOUS_DEVELOPMENT.md). What's still missing is a
> general per-workflow trigger: no `aimee workflow run` command and no Run button
> for an arbitrary (non-`build`) run. See [Limitations](#current-limitations).

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
| `gate.human` | approval | proposal · plan · branch · frozen_diff · pr | **gate** | Parks for a human decision (or auto-passes when preauthorized). |
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
panelists approve (or fails after `max_rounds`).

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
- A **human gate** **parks** the run (`WFE_STEP_PENDING`) until an approval is
  recorded. In `autonomous` mode the driver
  ([wfe_autonomy.c](../src/workflow/wfe_autonomy.c)) may auto-satisfy a gate that
  is explicitly `policy: preauthorized` or `optional: true`; otherwise it parks.
- Approvals are **signed** ([wfe_approval.c](../src/workflow/wfe_approval.c)) and
  recorded against the step's content hash, so an approval is bound to the exact
  artifact it approved.

A step resolves the working repository from `$AIMEE_WORKFLOW_REPO` (or the
process cwd), see [Limitations](#current-limitations) for the
run-in-a-specific-project gap.

## Inspecting runs

When work-items exist, they are readable (the Workflow Actions tab (see [`WORKFLOW_ACTIONS.md`](WORKFLOW_ACTIONS.md)) and
the API):

- CLI: `aimee cancel <work-item-id>` cancels a run.
- Webchat: `GET /api/workflow/items` (and `/items/<id>`) list run state; a run
  bound to a chat channel can be paused via
  `POST /api/sessions/workflows/<id>/pause`.
- Server `/v1/workflow/*` ([server_workflow_api.c](../src/server/server_workflow_api.c))
  exposes the def read/author surface and the work-item read surface.

## Current limitations

These are real today and worth knowing before you lean on workflows:

1. **Only the autonomous-development trigger is wired.** `POST /v1/dev/submit`
   creates and starts a run via `wfe_work_item_create` + the scheduler (see
   [Autonomous Development](AUTONOMOUS_DEVELOPMENT.md)). There is still no general
   per-workflow trigger, no `aimee workflow run` command and no Run button to kick
   off an arbitrary saved workflow from the UI. You can author, validate, save, and
   inspect any workflow; only `build`-style autonomous runs (submitted from the
   Workflow Actions tab) start today.
2. **Run-in-a-specific-project isn't wired.** A work-item has a `repo` field, but
   the per-step blocks resolve their working directory from
   `$AIMEE_WORKFLOW_REPO`/cwd rather than the work-item's `repo`, so binding a run
   to a UI-selected project is incomplete.
3. **Composer ergonomics.** New steps are added disconnected; you wire order in
   the inspector. There is no run/visualize-progress view because runs can't be
   started from the UI yet.

## See also

- [Personas](personas.md), the identities that staff roundtable panels and steps.
- [Delegates](DELEGATES.md), how steps dispatch model work.
- [Architecture](ARCHITECTURE.md), where the workflow engine sits.
