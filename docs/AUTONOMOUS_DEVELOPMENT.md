# Autonomous development

Autonomous mode lets the workflow scheduler run ordinary steps without waiting for an operator. It
does not widen tool, workspace, credential, budget, or gate authority.

## Start

Use Workflow Actions, a trigger, cron rule, or the typed submission API. Every path creates the same
durable work item with:

- an immutable admitted request and proposal;
- a named, versioned workflow;
- a repository and worktree authority;
- retry, round, agent, and spend limits;
- the initiating principal and audit context.

## Lifecycle

The default build path is:

```text
understand/proposal -> feature branch -> plan -> plan review
                    -> split -> parallel slice workflows
                    -> acceptance review -> documentation review
                    -> final pull request
```

Each slice gets a branch and worktree, implements one packet, verifies it, freezes the diff, and
runs review before merge into the feature branch. New slices branch from the current merged feature
tip.

The final PR targets the forge's authoritative default branch. The default workflow opens it and
stops; normal repository review controls the merge.

## Persistence and recovery

The Go control plane writes a lifecycle transition before external dispatch. Provider, delegate,
runner, or compatibility-worker failure cannot erase the authoritative state.

On restart, the scheduler:

1. reads active and parked items;
2. reconciles in-flight reservations and artifacts;
3. retries work that is safe to repeat;
4. parks anything that needs a decision or manual repair.

It never converts a missing result into success.

## Gates

- roundtable gates require valid evidence and quorum;
- CI gates fail closed when required checks are not green;
- mergeability gates stop on a conflict;
- delivery gates enforce the current verdict;
- human gates always park for a signed person.

Review findings persist and feed the next authoring or implementation pass. Repeated identical
feedback and output parks as no progress instead of consuming the full budget forever.

## Limits

Autonomy is bounded by:

- global and per-workflow agent slots;
- per-node retry and round counts;
- fan-out packet count;
- provider rate and quota;
- per-run spend;
- tool and process timeouts;
- worktree and branch confinement;
- trigger concurrency;
- sandbox network, package, and credential policy.

Fields under `autonomy.*` tune these limits. Some load at workflow-process startup. See
[Configuration](gen/configuration.md).

## Park reasons

| Reason | What to do |
| --- | --- |
| `human_gate` | inspect the exact artifact and sign approve/reject |
| `panel_degraded` | restore eligible reviewers or change the named preset |
| `convergence_limit` | inspect the last blockers and refine the request/workflow |
| `convergence_no_progress` | break the repeated plan/feedback cycle |
| `dependency_pending` | no action; the scheduler resumes the slice after its declared predecessors are accepted |
| agent capacity | wait, cancel stale work, or raise a safe limit |
| missing commit | repair the delegate result; do not advance an empty implementation |
| merge conflict | resolve against the current feature tip and rerun verification |
| CI failure | inspect the failing check; retry only after a change or transient diagnosis |
| forge failure | restore repository identity, credential, branch, or network authority |
| spend limit | raise it explicitly or narrow the work |

## Security

- The workflow process does not own agent credentials.
- Forge operations are typed and confined to managed worktrees and branch namespaces.
- Delegates retain their normal sandbox and source authority.
- Remote workspace mutation requires full per-user write authority.
- Tool and credential activity is audited through the event bus.
- Autonomous mode cannot approve a human gate.

## Observe

Use Workflow Actions for live state and artifacts. Use `aimee trigger status`, `aimee jobs`, server
logs, provider diagnostics, and `aimee audit verify` for the surrounding resource plane.

The event bus records migrated action paths, but workflow trigger delivery is not yet an integrated
bus consumer. See [Event bus](EVENT_BUS.md).

See [Workflows](WORKFLOWS.md), [Workflow Actions](WORKFLOW_ACTIONS.md), and
[Delegate sandbox](DELEGATE_SANDBOX.md).
