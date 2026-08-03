# Autonomous development

Autonomous mode lets the workflow scheduler run ordinary steps without waiting for an operator. It
does not widen tool, workspace, credential, budget, or gate authority.

## Start

Use Workflow Actions, a watched-proposal trigger, the workflow CLI, or the typed submission API.
Every path creates the same durable work item with:

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

The final PR targets the branch checked out when the repository was admitted. This integration lane
can intentionally differ from the forge's default branch. The default workflow opens the PR as a
draft and stops. Its title comes from the admitted proposal, and its body carries the original
request, approved plan, changed-file summary, slice PRs, completed workflow gates, and explicit
human-review instructions. Automation must not mark this PR ready, approve it, or merge it.

Draft state makes the handoff explicit and prevents an ordinary accidental merge, but it is not a
human identity system. A production deployment must also keep the workflow forge credential away
from general-purpose agents and enforce the repository's human-review policy. If a human and an
agent share the same writable GitHub identity, GitHub cannot prove which one performed an action.

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
- human gates always park for an explicit browser or API decision.

The current decision record is a hashed approval artifact plus a lifecycle transition. It is not a
cryptographic signature binding the principal to the reviewed artifact, so repository protection
and audit policy remain the authoritative human-review controls.

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

Fields under `autonomy.*` tune these limits. The browser's **Run policy** panel updates the live Go
workflow service. See [Configuration](gen/configuration.md).

## Park reasons

| Reason | What to do |
| --- | --- |
| `human_gate` | inspect the run evidence, then approve or reject in Workflow Actions or through the API |
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
- Autonomous mode cannot mark a final workflow PR ready or merge it.

## Observe

Use Workflow Actions for live state, proposal text, and lifecycle events. Use the CLI, server-side
artifact store, `aimee trigger status`, `aimee jobs`, server logs, provider diagnostics, and
`aimee audit verify` for deeper evidence and the surrounding resource plane.

The event bus records migrated action paths, but workflow trigger delivery is not yet an integrated
bus consumer. See [Event bus](EVENT_BUS.md).

See [Workflows](WORKFLOWS.md), [Workflow Actions](WORKFLOW_ACTIONS.md), and
[Delegate sandbox](DELEGATE_SANDBOX.md).

## Automatic proposal admission

Automatic proposal admission follows the canonical contract in
[Automatic proposal admission](wfe-autonomy-runbook.md#automatic-proposal-admission).
