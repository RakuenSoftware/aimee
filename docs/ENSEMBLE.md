# Roundtables and ensembles

Use an ensemble when independent answers improve the result. Use a roundtable when reviewers need a
shared artifact, named lenses, evidence, and a verdict.

## Aggregate

An aggregate run sends the same task to several eligible agents, then asks an aggregator to produce
one answer. It is useful for drafting, research, and questions with several plausible approaches.

```bash
aimee ensemble aggregate --help
```

The run keeps partial-failure metadata. A missing seat does not become an empty vote. The aggregator
sees the successful answers and the declared degradation.

## Roundtable

```bash
aimee ensemble roundtable --help
```

A roundtable has:

- a named preset;
- seats with personas;
- a pinned model or `$random` per seat;
- a review or drafting mode;
- parallel or sequential turns;
- quorum, round, deadline, and cost limits;
- an optional reasoning chair.

Parallel review is the normal path. Every lens runs at once under the compute and agent-admission
limits. Sequential turns are for discussions where a later seat must read earlier arguments.

## Seat selection

A pinned seat is a hard requirement. If its agent is disabled, unroutable, saturated, or fails, the
seat fails. aimee does not substitute a different model and pretend the panel stayed the same.

A random seat chooses any viable review-capable agent not already seated. A failed random seat may
retry on another eligible agent. Agents whose role list excludes `review` are never seated.

If the required panel cannot be formed, a workflow gate parks as `panel_degraded`.

## Evidence and verdicts

Reviewers must point to repository evidence: paths, symbols, diffs, tests, logs, or a specific
contract. Unsupported findings are weak evidence, not blockers.

The chair receives the seat outputs and can:

- merge duplicates;
- remove claims without evidence;
- distinguish blockers from debt;
- preserve disagreement;
- state the smallest change needed for approval.

An unusable response abstains. It is never counted as approval. Quorum is evaluated from valid
verdicts only.

## Workflow gates

`gate.roundtable` names a preset in `params.roundtable` and binds a proposal, plan, or frozen diff.
The gate persists its findings. A failed gate sends those findings back to the authoring or
implementation loop so the next pass addresses the actual objections.

Each gate may use a different preset: plan, security, acceptance, and documentation reviews need not
share a panel.

## Cost and audit

Seat usage, latency, cost, retries, model identity, evidence, chair output, and final verdict belong
to the originating session or workflow. A positive cost ceiling stops dispatch before the next seat
would exceed it; zero means no ensemble-specific ceiling.

Tool and model activity follow the normal audit path. Roundtable artifacts remain distinct from the
proposal, plan, and diff they reviewed.

See [Personas](personas.md), [Delegates](DELEGATES.md), and [Workflows](WORKFLOWS.md).
