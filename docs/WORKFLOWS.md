# Workflows

A workflow is not a list of steps. Control edges decide what runs; input bindings decide what each
step reads. Confusing the two produces a graph that looks connected and fails validation.

Use **Edit Workflows** to define the graph. Use **Workflows** to submit a proposal and operate the
run. Definitions are instance-wide files under `$AIMEE_HOME/workflows`. The project picker changes
browser context, not the definition namespace.

All signed-in users can inspect definitions and validate an in-browser draft. Because definitions
and custom blocks affect every future run on the instance, only the appliance administrator can
persist, delete, or edit them globally. The editor marks persistence and custom-block mutation
controls unavailable for other users, who may still inspect or validate an unsaved local draft;
both the web proxy and workflow service enforce the global-write boundary.

![The Edit Workflows page with a four-node review graph, the block palette, and colored transition edges](images/workflow-editor-graph.png)

## Choose a shipped workflow

The repository currently ships these definitions in [`config/workflows/`](../config/workflows/):

| Workflow | Use it for | Current path and terminal effect |
| --- | --- | --- |
| `build` | a full proposal-to-PR change | draft proposal, open a feature branch, plan, review the plan, split into child slices, review the assembled change, document it, then open a draft PR |
| `build-triggered` | the same lifecycle started by a watched proposal directory | starts at `trigger.watch-dir`; it is disarmed until that node has `params.workspace` |
| `slice` | one packet created by `build` | implement, freeze, roundtable review, open a slice PR, wait for CI, then merge into the parent feature branch |
| `managed-change` | a substantive manager-led change | understand, split, implement, review, roundtable, then `gate.deliver`; it does not open a PR |
| `hotfix` | a small change with the standard review quorum | understand, implement, review, roundtable, then `gate.deliver`, with shorter review budgets |
| `manual-review` | untrusted or machine-proposed input that must not implement itself | normalize the proposal and stop at `gate.human`; approval ends this run and does not automatically start `build` |

The root `build` workflow opens a draft PR and stops. A human must review it, mark it ready, and
decide whether to merge. A root PR with `params.base: trunk` or `default` targets the branch checked
out when the repository was admitted. That integration branch can intentionally differ from
`origin/HEAD`. Slice PRs use `params.base: feature` and are the only PRs the workflow may merge
automatically.

## Understand the graph

A definition contains a start node and a list of nodes. Each node names a block and can have four
kinds of configuration:

| Field | Meaning |
| --- | --- |
| `in` | typed data bindings from another node's single `out` port |
| `params` | block-specific settings such as persona, roundtable, quorum, or `max_rounds` |
| `next` | ordinary successful transition |
| `on_pass` / `on_fail` | pass and requested-change transitions, normally used by gates and reviews |

Control edges decide which node runs next. Input bindings decide which persisted artifact that node
reads. These are separate relationships. For example, a gate can run after `plan` through a control
edge and read `plan.out` through `in.src`.

The canvas renders `next` in gray, `on_pass` in green, `on_fail` in red, and input bindings as a
lighter dotted line. Dragging a card only changes the current canvas layout. Coordinates are not part
of the definition, so the editor lays out the graph again when it is reopened.

## Build a graph in the browser

1. **Open the graph.** Open a saved workflow or select **+ New**.
2. **Add a block.** The new node starts disconnected.
3. **Set its work.** Select the node and set its title, task, persona, and optional delegate.
4. **Mark the entry.** Select **set as start** on the first node.
5. **Bind its inputs.** Under **Inputs**, connect every required port to a producer.
6. **Set its control edges.** Choose `next`, `on_pass`, and `on_fail` targets.
7. **Add raw parameters.** Use **Advanced (raw params)** for fields without a dedicated control.
8. **Validate the graph.** Resolve every port, type, parameter, block, and edge error.
9. **Save the definition.** If another editor saved first, reopen and reapply the change
   against the new version.

The editor saves `name`, `start`, and `nodes`. It does not currently preserve top-level
`intent_tags` or `enforced`. Edit definitions that rely on those fields as YAML instead of saving
them through the visual editor.

## Create a bounded loop

The arrow is easy. The budget is the loop.

A backward control edge retries the same node or returns to an earlier one. Use a self-loop for a
transient retry. Send requested changes to an author or implementation node when review needs a new
artifact before it runs again.

In the example below, **Quality gate** advances to `deliver` on approval and returns to `plan` when
the panel requests changes. The inspector exposes those targets directly.

![The workflow node inspector showing on_pass set to deliver and on_fail set to plan](images/workflow-editor-loop.png)

To build this loop in the browser:

1. **Select the decision node.** Choose the review or gate.
2. **Set the pass edge.** Point **on_pass** at the stage after approval.
3. **Set the change edge.** Point **on_fail** at the node that can address the feedback.
4. **Bound the loop.** Add a positive `max_rounds` value to the node that returns the
   requested-change result. For a plan-to-gate refinement loop, put it on the gate.
5. **Check the data edge.** Bind the new author output into the gate, then validate and save.

This is a complete YAML version of that graph:

```yaml
name: review-loop
start: draft
nodes:
  - id: draft
    block: author.proposal
    next: plan
    on_fail: draft
    params:
      max_rounds: 3

  - id: plan
    block: author.plan
    in:
      proposal: draft.out
    next: quality_gate
    on_fail: plan
    params:
      max_rounds: 3

  - id: quality_gate
    block: gate.roundtable
    in:
      src: plan.out
    params:
      roundtable: default
      panel:
        required:
          - security
          - qa
          - reviewer
      quorum: 3
      max_rounds: 6
      focus: does this plan satisfy the proposal?
    on_pass: deliver
    on_fail: plan

  - id: deliver
    block: gate.deliver
    in:
      verdict: quality_gate.out
```

The current validator permits cycles and does not require an explicit cycle budget. At runtime,
`max_rounds` is the per-node repeat limit. A missing, zero, or negative value uses the default of 20.
Do not use the retired name `max_iters`.

When a review or roundtable keeps requesting changes, exhaustion parks the run as
`convergence_limit`. Three repeated rounds without changed review progress can park earlier as
`convergence_no_progress`. Other change/retry paths park as `retry_limit` when their node budget is
exhausted. A runner error, terminal block failure, or pending external condition can park or reject
without following `on_fail`, so `on_fail` is not a general exception handler. Run-level spend, turn,
wall-clock, concurrency, and resume limits remain additional backstops.

## Compose a graph from child workflows

`foreach.workflow` calls another saved graph once for each packet. The parent waits until every child
is accepted, then emits the assembled feature branch.

![A foreach.workflow node selected in Edit Workflows with its child workflow and fan-out settings](images/workflow-editor-foreach.png)

```yaml
  - id: split
    block: split
    in:
      plan: plan.out
    next: slices
    on_fail: split

  - id: slices
    block: foreach.workflow
    in:
      packets: split.out
      feature: feature.out
    params:
      workflow: slice
      max_children: 8
      max_rounds: 3
    next: acceptance
    on_fail: split
```

The two required ports are `packets` from a `plan` artifact and `feature` from a `branch` artifact.
`params.workflow` defaults to `slice`; set it explicitly for a different child definition. The
default `max_children` is 16. Too many packets parks the parent as `fanout_limit`. A failed child
returns a change result to the parent's `on_fail` edge, where the example regenerates the packet set.

In the visual editor, a `foreach.workflow` node is drawn as a stacked callout. Choose the child in
the inspector and use **open** to move to that definition. The picker excludes the workflow currently
being edited.

## Definition and validation contract

The Go workflow service owns definitions, canonical version hashes, scheduling, durable state,
worktrees, gates, forge operations, and recovery. A run pins both the exact definition and resolved
block catalog it started with. Later edits affect new runs only.

Validation currently checks:

- **Document shape:** one YAML document with known fields.
- **Graph identity:** a non-empty name, at least one node, and unique IDs matching
  `[A-Za-z][A-Za-z0-9_-]*`.
- **Control targets:** an existing start node and existing transition targets.
- **Block contracts:** known blocks, required parameters, and required input ports.
- **Data bindings:** `producer.out` syntax, an existing producer, and an accepted artifact type.
- **Panel shape:** valid roundtable personas and quorum.

Validation does not prove that every node is reachable, require a cycle budget, or judge whether a
loop can make semantic progress. Make the start explicit and inspect both sides of every loop.

## Built-in blocks

Run `aimee workflow blocks` for the installed catalog. The current built-ins are:

| Block | Required input port and accepted artifact | Produces |
| --- | --- | --- |
| `trigger.watch-dir` | none | `proposal` |
| `author.proposal` | none; optional `proposal` | `proposal` |
| `understand` | none | `intent` |
| `author.plan` | `proposal` | `plan` |
| `split` | one of `intent` or `plan` | `plan` |
| `branch.open` | none | `branch` |
| `implement` | `plan`, accepting `plan` or `intent` | `branch` |
| `foreach.workflow` | `packets` as `plan` and `feature` as `branch` | `branch` |
| `document` | `branch` | `branch` |
| `source.archive` | `branch` | `branch` |
| `freeze` | `branch` | `frozen_diff` |
| `review` | `src` as `frozen_diff` or `branch` | `verdict` |
| `gate.roundtable` | `src` as `proposal`, `plan`, or `frozen_diff`; `roundtable` param | `verdict` |
| `gate.human` | `src` as `proposal`, `plan`, `branch`, `frozen_diff`, or `pr` | `approval` |
| `check.mergeable` | `pr` | `verdict` |
| `gate.ci` | `pr` | `verdict` |
| `gate.deliver` | `verdict`, accepting a `verdict` or `approval` artifact | `none` |
| `pr.open` | `src` as `proposal` or `frozen_diff` | `pr` |
| `merge` | `pr` | `none` |

## Custom blocks

Custom blocks live in `$AIMEE_HOME/workflows/blocks.yaml` and cannot shadow a built-in. The browser's
**Blocks + New** form creates delegate blocks with a persona, prompt, consumed artifact type, and an
output of either `branch` or `none`. A consuming custom block uses the input port named `in`.

![The custom delegate block form with consumed and produced artifact types, persona, and prompt](images/workflow-editor-custom-block.png)

Operator-managed command blocks are YAML-only. They remain disabled unless `allow_command` is set,
and require an absolute executable, a matching SHA-256 digest, and a bounded timeout.

Editing a custom block creates a new resolved execution version for future runs. A run already in
progress retains the old prompt and block contract.

## Start and inspect a run

The local definition commands use the legacy C catalog and read files from disk or
`$AIMEE_HOME/workflows`. The `run` and `status` commands use the Go server. Browser Save and Validate
are authoritative for a definition that the Go engine will execute.

```bash
aimee workflow blocks
aimee workflow new ~/.config/aimee/workflows/review-loop.yaml
aimee workflow validate ~/.config/aimee/workflows/review-loop.yaml
aimee workflow show ~/.config/aimee/workflows/review-loop.yaml
aimee workflow list

aimee workflow run review-loop \
  --proposal docs/proposals/pending/change.md \
  --repo /srv/repos/project \
  --watch
aimee workflow status <work-item-id> --events
```

The browser proposal composer also starts a run, but the current submit path always records it as
`autonomous` and requires a repository path visible to the server. See [Workflow Actions](WORKFLOW_ACTIONS.md)
for its exact controls.

## Configure triggers

The current Go trigger scanner supports `watch-dir` and its compatibility spelling `proposals`.
Other source names can be displayed by the UI but are reported as unsupported by this scanner.

You can configure a watched directory in either place:

- select **Workflows → Triggers → + New trigger** as the appliance administrator, then choose the
  repository or an absolute server-visible checkout path, saved workflow, watched directory,
  optional branch/ref, mode, and spend cap;
- add a rule under `trigger_rules` in `aimee.yaml` when repairing an invalid registry or managing it
  as configuration;
- make `trigger.watch-dir` the workflow's start node and set its `params.workspace`.

The browser keeps a new trigger as a draft until an explicit save succeeds, uses optimistic
versioning to avoid overwriting another operator, and makes graph-native triggers read-only. Other
users can inspect automatic starts but cannot change the global registry. The registry accepts at
most 32 rules and rejects repository traversal, option-shaped Git refs, invalid run modes, and
negative or non-finite spend caps.

A graph-native trigger without `params.workspace` is saved but disarmed. `params.dir` defaults to
`docs/proposals/pending`; `params.ref` defaults to the refreshed remote default ref; and
`params.max_spend_usd` sets the admitted run's spend ceiling. The scanner reads committed, visible
Markdown files from the selected git ref, deduplicates the proposal bytes with workflow and mode,
and retries admission on a later scan when the concurrency cap is full.

The definition validator checks graph-native trigger parameter types, directory confinement, Git
ref safety, run mode, and spend cap when the workflow is authored. Invalid trigger nodes therefore
fail **Validate** or **Save** instead of becoming a repeating scanner error.

Setting `trigger.max_concurrent` to `0` pauses new trigger and browser-submit admission. It does not
remove the cap.

Trigger mode is persisted, but the current Go scheduler does not branch its advancement behavior on
`interactive` versus `autonomous`. Use `gate.human` or pause the run when operator approval must be a
hard boundary.

## Operate and recover

The engine persists transition and cost evidence around each external action. It can resume after a
process restart without switching an active run to a newly edited definition. Common safe stops
include human gates, CI or merge pending, roundtable availability, retry or convergence limits,
fan-out limits, spend limits, and integration conflicts.

Use the run's current stage, pause reason, version, timeline, and proposal before deciding to resume,
stop, or change the definition. See [Workflow Actions](WORKFLOW_ACTIONS.md) for browser controls,
[Autonomous development](AUTONOMOUS_DEVELOPMENT.md) for the build lifecycle, and the
[autonomy runbook](wfe-autonomy-runbook.md) for recovery.
