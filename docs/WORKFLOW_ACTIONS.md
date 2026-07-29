# Workflow Actions

The browser's **Workflow Actions** page starts and operates workflow runs. **Edit Workflows** changes
definitions; it does not operate live work.

## Start a run

Choose:

- project/repository;
- saved workflow;
- proposal or request;
- interactive or autonomous mode;
- optional spend ceiling.

Submission validates the input, creates the durable work item, snapshots the workflow, and returns
the item ID. The request, proposal, plan, implementation, reviews, and documentation remain separate
artifacts.

## Read a run

The page shows:

- state and current node;
- feature and slice branches;
- latest artifact and content hash;
- delegates, retries, and admission waits;
- roundtable evidence and verdicts;
- spend and configured limit;
- CI, merge, forge, and park state;
- the append-only lifecycle history.

The UI polls or streams read models from the Go workflow API. It does not reconstruct state from
browser events.

## Act on a run

Depending on state, an authorized user can:

- start or resume scheduling;
- pause or cancel;
- approve or reject a human gate;
- retry a recoverable parked step;
- open the current artifact or forge link.

A gate decision is bound to the principal, node, and current artifact hash. If the artifact changes,
the old decision cannot advance the run.

Autonomous mode never passes a human gate. It only drives non-human steps until completion, failure,
or a named park.

## Authorization

Reads require workflow-read authority. Starting, retrying, canceling, or deciding a gate requires the
corresponding workflow-admin capability and user write grant. Browser project selection is not an
authorization grant; the owning service checks every request.

Forge credentials stay in the server vault. The workflow process requests a narrow mechanical forge
operation and never receives the secret.

## Failure states

The page keeps the reason instead of flattening every stop into “failed.” Common states include:

- human approval required;
- no eligible agent or agent limit reached;
- panel degraded or no valid quorum;
- convergence limit or repeated no progress;
- missing commit or empty implementation;
- verification failure;
- merge conflict;
- lost review replay;
- forge or CI failure;
- spend limit reached.

Fix the named condition, then use the offered action. Do not restart a new run merely to lose the
evidence from the first one.

See [Workflows](WORKFLOWS.md) and [Autonomous development](AUTONOMOUS_DEVELOPMENT.md).

## Automatic proposal admission

See [Automatic proposal admission](wfe-autonomy-runbook.md#automatic-proposal-admission) for the five behaviors that determine when the autonomous pending-proposal watcher makes a pending proposal eligible for a new run.
