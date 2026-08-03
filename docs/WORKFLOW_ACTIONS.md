# Workflow Actions

The browser's **Workflows** page starts proposals and operates saved runs. **Edit Workflows** changes
the graph used by future runs. Editing a definition does not mutate the version pinned to an existing
run.

![The Workflows page showing a run waiting at a human gate, its controls, event history, and proposal](images/workflow-actions.png)

## Start a run

1. Select **+ New proposal**.
2. Enter a title and proposal body, then choose a saved workflow.
3. Choose a managed repository or enter a custom checkout path visible to the server. The form
   requires this before it sends the proposal.
4. Optionally use **Draft with a delegate** or **Load from project**. Both show a preview before
   replacing the proposal body.
5. Select **Submit**. The page opens the new run and begins polling it.

The in-progress draft is stored in browser local storage and cleared at logout. Browser submission
currently creates an `autonomous` run. The composer does not expose trigger mode or a per-run spend
ceiling; a watched trigger can set `max_spend_usd` instead.

The CLI equivalent is:

```bash
aimee workflow run build \
  --proposal docs/proposals/pending/change.md \
  --repo /srv/repos/project \
  --watch
```

## Read a run

The left rail lists runs and their derived status labels. Selecting one shows the current:

- state, stage, workflow, and pinned version;
- repository, accumulated cost and optional cap;
- PR reference when one has been created;
- submitter and override count when present;
- append-only lifecycle events with stage, actor, detail, timestamp, and step cost;
- immutable admitted proposal.

The selected run is polled every four seconds while it remains open. Event reads use an incremental
cursor, so the page appends new history instead of reconstructing state from browser events.

The current page does not expose every stored plan, diff, review, branch, slice, or worktree artifact.
Use the PR, event detail, CLI status, and server-side artifact store when that deeper evidence is
needed.

## Act on a run

Controls depend on the current state:

| Control | Current behavior |
| --- | --- |
| **Pause** | parks an unpaused active run and cancels its current scheduler dispatch |
| **Start** | resumes a non-human paused run |
| **Approve** | resolves `gate.human`, writes an approval artifact, and follows `on_pass` or `next` |
| **Reject** | follows the human gate's `on_fail` edge or ends the run as rejected when no edge exists |
| **Stop** | stops the root and descendants; a stopped run cannot be resumed |
| **Delete** | stops active descendants, removes run artifacts and managed worktrees, then permanently deletes the run tree and history |

There is no separate **Retry** button in the current page. Fix the named condition and use **Start**
when the pause reason is resumable. A human gate must use **Approve** or **Reject** instead.

The current human-gate record is a hashed approval artifact plus lifecycle transition. It is not a
signed principal-and-artifact attestation. Do not treat it as a cryptographic approval record.

## Understand the build handoff

The root `build` workflow ends after opening a draft PR against the admitted repository checkout's
integration branch. Its PR body includes the original request, approved plan, changed-file summary,
slice PRs, and completed review gates. The workflow does not mark that PR ready or merge it.

Child `slice` workflows are different. After roundtable approval and green CI, their PRs may merge
only into the parent's `aimee/feat/...` branch. They cannot autonomously target the repository's
integration branch.

## Configure automatic starts

The **Triggers** panel shows what can file a run automatically.

- The appliance administrator can select **+ New trigger** and choose a managed repository (or
  enter a custom server-visible checkout path), a saved workflow, the repository-relative
  Markdown directory to watch, and an optional branch/ref, run mode, and per-run spend cap.
- New and edited rules stay as browser drafts until **Save trigger** succeeds. The form validates
  repository confinement and Git-ref safety before changing the shared registry, and a concurrent
  edit reloads the current registry instead of overwriting it.
- Non-administrators can inspect active triggers but do not see mutation controls. A malformed
  on-disk registry is reported and held read-only so the browser cannot erase rules it failed to
  parse.
- A trigger originating from a saved workflow's `trigger.watch-dir` start node is read-only in this
  panel. Edit that node in **Edit Workflows** to change or disarm it.
- The current Go scanner creates `watch-dir` rules; `proposals` remains a compatibility spelling for
  existing configuration. Other source names are not offered by the browser because this scanner
  cannot execute them.

A watch rule needs a server-visible workspace, a saved workflow, a repository-relative proposal
directory, and an optional git ref. Leaving the ref blank resolves the refreshed remote default ref.
The browser offers valid saved workflows and repositories already managed for the signed-in
administrator, while retaining a custom path option. The shared registry is limited to 32 rules.
The mode and optional spend cap are stored with the admitted run, but mode alone is not an approval
barrier in the current scheduler. Put `gate.human` in the graph when a person must decide.

## Tune run policy

The collapsible **Run policy** panel writes live workflow configuration. The bootstrap administrator
is the only browser identity allowed to save these global values.

Current defaults include:

| Setting | Default | Effect |
| --- | ---: | --- |
| trigger admission cap | 2 | maximum active root runs admitted by triggers and manual submit |
| proposal scan interval | 5 seconds | delay between watched-directory scans |
| workflow concurrency | 5 | scheduler work driven concurrently across the instance |
| maximum turns | 300 | cumulative runaway backstop for one run |
| wall time per resume | 1,800 seconds | parks a run when one resume window expires |
| maximum automatic resumes | 50 | bounds wall-cap auto-resume |
| stale-abandon grace | 3,600 seconds | reaps a stale capped or stuck park |
| unassigned delegate lease | 120 seconds | cancels and safely retries a job with no eligible agent |

Node-level `max_rounds` is defined in the graph and is separate from these run-level limits. See
[Workflows](WORKFLOWS.md#create-a-bounded-loop).

## Diagnose a parked or terminal run

Read the pause reason and the last events before acting. Common current reasons include:

| Reason | What to check |
| --- | --- |
| `human_gate` | review the proposal and current stage, then approve or reject |
| `manual` | confirm why the operator paused it, then start or stop it |
| `ci_pending`, `merge_pending`, `slices_running` | inspect the external PR, checks, or child runs; the scheduler normally redrives these |
| `panel_unreachable`, `roundtable_discussion`, `roundtable_chairman` | restore the named roundtable or its eligible agents |
| `request_unimplementable` | resolve the contradiction or missing prerequisite named by the chairman |
| `retry_limit`, `convergence_limit`, `convergence_no_progress` | inspect the repeated blocker and change the input, node task, or loop before resuming |
| `fanout_limit` | reduce packets or raise the node's bounded `max_children` |
| `budget_cap`, `turn_cap`, `wall_cap` | inspect cost and run policy before allowing more work |
| `base_integration_conflict` | update or resolve the integration branch before resuming |
| `workflow_definition_invalid`, `workflow_block_unavailable` | restore the pinned definition or block version; editing only the current definition is insufficient |

Do not file a replacement run merely to hide the evidence on the first one. Stop only when the run
should not continue, and delete only when its proposal, artifacts, descendants, and history are no
longer needed.

See [Workflows](WORKFLOWS.md), [Autonomous development](AUTONOMOUS_DEVELOPMENT.md), and the
[autonomy runbook](wfe-autonomy-runbook.md). Automatic watched-proposal admission is specified in
[Automatic proposal admission](wfe-autonomy-runbook.md#automatic-proposal-admission).
