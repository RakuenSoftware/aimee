# Workflows

A workflow is a typed graph that turns an admitted request into versioned artifacts and, normally,
a pull request. The Go control plane owns definitions, scheduling, retries, gates, worktrees, forge
operations, and recovery.

## The contract

A definition has:

- a name and start node;
- nodes, each naming one block;
- control edges: `next`, `on_pass`, and `on_fail`;
- typed input bindings from an earlier node's output;
- bounded parameters for retries, fan-out, panels, and spend.

The validator rejects unknown blocks, dangling edges, cycles without a declared budget, missing
ports, and artifact type mismatches. The canonical definition is hashed. A run keeps the exact
snapshot it started with even if the file changes later.

```yaml
name: review-change
start: proposal
nodes:
  - id: proposal
    block: author.proposal
    next: plan
    on_fail: proposal

  - id: plan
    block: author.plan
    in:
      proposal: proposal.out
    next: review
    on_fail: plan

  - id: review
    block: gate.roundtable
    in:
      src: plan.out
    params:
      roundtable: default
      quorum: 3
      max_rounds: 6
    on_pass: implement
    on_fail: plan

  - id: implement
    block: implement
    in:
      plan: plan.out
```

## Built-in blocks

Run `aimee workflow blocks` for the live catalog.

| Block | Purpose | Output |
| --- | --- | --- |
| `trigger.watch-dir` | file a run from a committed file | proposal |
| `author.proposal` | normalize or draft a proposal | proposal |
| `understand` | turn a request into bounded intent | intent |
| `author.plan` | plan against a proposal | plan |
| `split` | divide intent or plan into packets | plan |
| `branch.open` | create the durable feature branch | branch |
| `implement` | implement one plan | branch |
| `foreach.workflow` | run a child workflow for each packet | branch |
| `document` | update documentation on a branch | branch |
| `source.archive` | move the triggering source into its done location | branch |
| `freeze` | make an immutable review artifact | frozen diff |
| `review` | review a branch or frozen diff | verdict |
| `gate.roundtable` | convene a named panel | verdict |
| `gate.human` | park for a signed human decision | approval |
| `check.mergeable` | reject a conflicting PR | verdict |
| `gate.ci` | wait for required checks | verdict |
| `gate.deliver` | enforce the delivery verdict | none |
| `pr.open` | open a pull request | PR |
| `merge` | merge an approved PR | none |

## Default build

The shipped `build` workflow:

```text
request -> proposal -> feature branch -> plan -> plan roundtable
        -> split -> parallel slice workflows -> acceptance roundtable
        -> documentation -> documentation roundtable -> archive source
        -> open feature PR against the repository default branch
```

Each slice implements, freezes, reviews, verifies, and merges into the feature branch. The feature PR
is opened against the forge's authoritative default branch and left for normal human review.

Roundtable rejection feeds its blockers into the next author or split pass. The loop parks when it
repeats without progress or reaches its declared round budget.

## Run state

The control plane persists a transition before dispatching external work. A work item records:

- admitted request and immutable proposal;
- workflow name, version, and current node;
- repository, worktree, feature branch, and slice branches;
- plan, diff, review, documentation, and forge artifacts;
- retry, loop, agent, and spend budgets;
- state, park reason, gate evidence, and lifecycle events.

A provider, agent, runner, or compatibility worker can fail without corrupting the run. Restart
recovery reads the durable transition and retries or parks according to the node contract.

## Failure behavior

- A delegate failure follows `on_fail` or parks if the graph has no recovery edge.
- A missing implementation commit cannot advance.
- A merge conflict is terminal for that attempt.
- A slice branches from the current merged feature tip, not stale `origin/HEAD`.
- A lost roundtable replay invokes reservation recovery instead of pretending the review passed.
- A saturated agent is skipped; global and per-workflow limits stay enforced.
- Exhausted or repeated no-progress loops park with the last blocker.
- A malformed or unusable reviewer abstains; it is not counted as approval.

## Human gates

`gate.human` always parks. Autonomous mode cannot approve it, and validation rejects a supposedly
optional or preauthorized human gate.

The approval records the principal, node, current artifact hash, verdict, and signature. Editing the
artifact invalidates the decision.

An out-of-band forge review of the final PR is separate from `gate.human`; use either or both based
on the workflow.

## Triggers

Triggers create the same admitted work item as a browser submission. Sources include manual fire,
watch-directory/proposal scans, cron, and configured webhook sources.

```yaml
trigger:
  max_concurrent: 1

trigger_rules:
  - source: watch-dir
    event: docs/proposals/pending
    schedule: main
    mode: autonomous
    pipeline:
      template: build
      workspace: /srv/repos/project
      max_spend_usd: 5
```

`autonomous` starts scheduling immediately. `interactive` files the run and parks for a person.
Neither mode changes gates inside the workflow.

Watch-directory triggers read a named git ref, materialize the file, deduplicate it, bind the run to
the configured repository, and defer rather than drop when the trigger concurrency limit is full.

The current scheduler owns trigger delivery directly. Publishing all trigger firings through the
event bus is the next integration step, not an existing guarantee.

## Custom blocks

Custom blocks live in the workflow `blocks.yaml`. A block declares its input and output type, then
uses either a delegate or an operator-managed command.

Delegate blocks require a persona and prompt. Command blocks are disabled unless the operator opts
in; the executable must be absolute, hash-pinned, and bounded by a timeout. A custom block cannot
shadow a built-in.

## Author and operate

```bash
aimee workflow blocks
aimee workflow new ~/.config/aimee/workflows/change.yaml
aimee workflow validate ~/.config/aimee/workflows/change.yaml
aimee workflow show ~/.config/aimee/workflows/change.yaml
aimee workflow list

aimee trigger fire --help
aimee trigger list
aimee trigger status <id>
aimee trigger cancel <id>
```

The **Edit Workflows** page edits definitions. **Workflow Actions** starts, watches, parks, resumes,
and gates runs. A new visual node is disconnected until its control and data edges are set.

## Boundaries

- Go is the only workflow lifecycle writer.
- The C server owns agents, credentials, general tools, and policy resources used by a run.
- Forge operations are confined to the managed worktree root and exact feature/slice namespace.
- Credential material never returns to the workflow process.
- The event bus is the internal observability spine, but workflow trigger events are not yet on the
  integrated bus path.

See [Autonomous development](AUTONOMOUS_DEVELOPMENT.md), [Workflow Actions](WORKFLOW_ACTIONS.md),
and [Roundtables](ENSEMBLE.md).
