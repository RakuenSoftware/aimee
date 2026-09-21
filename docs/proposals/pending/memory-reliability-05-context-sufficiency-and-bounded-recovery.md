# MR-05: Requirement-based context sufficiency and bounded recovery

- **State:** In progress; nonempty typed results no longer claim complete task coverage
- **Priority:** P0 for honest sufficiency; P1 for recovery
- **Owner:** Go memory requirements and coverage, with host-governed recovery execution
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-03](memory-reliability-03-final-payload-context-budgets.md); [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) for independence requirements
- **Delivery:** Three implementation slices

## Problem and intended result

Typed context now separates retrieval availability from task coverage. Nonempty
results report unknown sufficiency until versioned requirements are evaluated;
an unavailable channel is distinct from a successful empty retrieval. This removes
the previous nonempty-to-complete shortcut but does not implement requirement
planning, final-host packing feedback, source-chain coherence or bounded recovery.
The benchmark result schema and temporal fixture validator accept `UNKNOWN`;
report buckets keep unassessed coverage separate from assessed missing evidence
for both correct and incorrect answers.

A nonempty result is not necessarily sufficient. A temporal question may require an old state and its correction; a comparison needs evidence for both sides; a current-state claim may require the latest applicable record. A maximum confidence value across retrieved records does not measure support for the combined answer.

Replace the nonempty/degraded shortcut with task requirements evaluated over the evidence that survived final serialization. Keep retrieval availability, requirement coverage and answer confidence separate.

## Requirement model

Create a bounded `EvidenceRequirementSet` with planner version, task revision, query mode, required roles, optional roles, conflicts and recovery budget. Roles include current state, historical predecessor, temporal anchor, source group, claim support, counterexample, active constraint and independently corroborating evidence.

Each requirement identifies an answer obligation rather than a benchmark answer. Example: “identify the database before the migration and the current database, with the change date if stated.” Do not inject known gold IDs into the serving planner. Model-proposed requirements are suggestions validated by the host; they do not grant access or invent authority.

Evaluate each requirement as `satisfied`, `missing`, `budget_dropped`, `unavailable`, `stale`, `conflicted` or `not_applicable`. Overall coverage is `complete`, `partial`, `insufficient` or `unknown`, with a reason list. Complete means the declared requirements are satisfied; it is not a guarantee that the planner was complete or the answer is correct.

## Assembly and recovery

Current/history evidence uses explicit source versions, valid time and belief time. Assemble coherent episode/update bundles where needed. Preserve user constraints and authoritative corrections even when they are frequent or less lexically similar.

After [MR-03](memory-reliability-03-final-payload-context-budgets.md) packing, evaluate the exact retained IDs and spans. If evidence is missing, choose an authorized recovery action based on the gap: retrieve a related source-chain member, expand a named span, seek the latest update or retrieve a narrowly relevant residual span omitted by extraction.

The recovery loop has explicit maximum rounds, new items, tokens, elapsed time and cost. Initial rollout allows at most one automatic recovery round; additional rounds require a new host-governed plan revision within operator ceilings. Exhaustion returns partial/insufficient and identifies the gap. Avoid repeatedly retrieving the same version with a different query.

Unavailable indexing and no matching evidence are distinct states. An unresolved authoritative contradiction cannot be resolved by selecting the highest confidence number. Surface both authorized alternatives or abstain according to task policy.

## Existing integration points and slices

1. Implement a shared coverage evaluator in the Go memory module, replacing the combined-answer confidence shortcut in `server-go/modules/memory/retrieval.go`. Convert `src/kb/db2_adapters/kb_service_backend_context.c` to consume that result and remove its count-based sufficiency decision. Preserve compatibility fields while adding reasoned coverage and an explicit `requirements_version`.
2. Add deterministic task templates for current-state, temporal change, comparison and procedure application. Join source-family/episode data and final packer selection. Unknown task shapes remain unknown rather than defaulting to complete.
3. Add one bounded memory recovery planner in Go that proposes expansions over existing memory, source-span and indexed-code tools. The host admits tool work under its existing task/access budgets and returns authenticated outcomes for coverage reevaluation. Record attempted expansions, results and remaining gaps. [MR-07](memory-reliability-07-task-exploration-contracts.md) uses these outcomes to govern supplementary exploration.

## Acceptance gates

- One high-confidence unrelated item never satisfies a multi-evidence task.
- Retrieving a required update and then dropping it during packing produces `budget_dropped`, not complete.
- Historical and current evidence cannot be joined into a false timeline through an unrelated episode.
- A revoked or unavailable source is not counted through an old cached projection.
- Independent-support requirements cannot be satisfied by copies of the same origin.
- Recovery terminates at its work/latency/token cap and records duplicate attempts.
- False-complete rate and answer quality are measured separately; all intentionally incomplete frozen fixtures must avoid complete status.

## Rollout and rollback

Ship truthful states before adding restrictive exploration. Shadow the requirement planner against reviewed fixtures and real task samples. Recovery starts opt-in with low bounded work. A rollback may disable automatic recovery but must retain honest missing/degraded states.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
