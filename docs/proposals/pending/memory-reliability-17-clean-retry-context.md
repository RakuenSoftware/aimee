# MR-17: Clean retry context with preserved real-world state

- **State:** Proposed
- **Priority:** P2: task recovery
- **Owner:** Turn runtime, context and existing workspace-recovery owner
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-03](memory-reliability-03-final-payload-context-budgets.md), [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md), [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md), [MR-13](memory-reliability-13-disposable-task-projections.md), [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md)
- **Delivery:** Three implementation slices

## Problem and intended result

Failed speculative reasoning can pollute later attempts. Resetting the prompt can remove that contamination, but it cannot undo a file edit, provider request or external side effect. Reusing an old snapshot can also reintroduce revoked evidence or obsolete user constraints.

Build retry context from a verified pre-attempt baseline plus current authoritative changes, a bounded failure summary and the durable action journal. Keep context isolation separate from workspace restore and external-effect reconciliation.

## Snapshot and retry contract

At attempt start, record a baseline context/plan reference, source versions, current user constraints, task projection revision, policy/scope generation, renderer version and receipt binding. Store content only under the existing governed replay/retention policy; a digest-only snapshot cannot promise reconstruction.

After failure, the host records failure class, actual tool/verifier evidence, unresolved gaps, attempted actions and known/unknown effects. A model may suggest a concise lesson, but the lesson remains derived and non-authoritative. Never preserve speculative claims merely because they appeared confidently in the failed attempt.

For retry:

1. Verify baseline integrity and retained-input availability.
2. Reauthorize source versions and apply current corrections, revocations and user instructions; do not restore obsolete authority.
3. Reconcile all durable actions since the baseline. Preserve changed object versions and unknown external outcomes.
4. Build a small failure summary with evidence references and prohibited-repeat conditions, within explicit token/byte limits.
5. Reassemble and budget the new context; issue new attempt, plan, receipt and exploration-contract revisions.

An external action with unknown outcome blocks unsafe replay until reconciled under [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md). A workspace change is restored only through the existing version-bound preview/restore workflow. “Retry from clean context” never implies “all effects were rolled back”.

## Retry limits

Set maximum attempts, total task tokens/cost, wall time and repeated-failure threshold in operator policy. The same failing plan cannot repeatedly reset its budget. Delegated attempts share the parent task ceiling. Give new attempts only the remaining budget, and record why an attempt was allowed or refused.

Do not blindly discard useful successful intermediate work. Host-verified results and admitted source changes remain available as new evidence. User interruptions and revised requirements take precedence over the old baseline.

## Implementation slices

1. Add baseline snapshot references and attempt-linked failure events to the existing turn state.
2. Implement clean reconstruction with current eligibility, action reconciliation and bounded negative lessons; connect it to task projection revisions.
3. Add bounded retry policy and integrate workspace restore as a separate explicit operation with its existing conflict checks.

## Acceptance gates

- Failed speculation is absent from the next attempt except for a bounded, labeled failure summary.
- Actual writes/effects survive in the action journal and cannot be accidentally replayed as if they never happened.
- Revoked evidence and replaced user constraints are not resurrected from a snapshot.
- Corrupt or unavailable snapshot inputs produce an explicit reconstruction failure and safe fresh-plan path.
- Repeated attempts cannot exceed the shared task ceiling, even across restarts/delegates.
- A failed branch does not erase host-verified successful work or claim a filesystem restore that was not executed.

## Rollout and rollback

Begin with read-only/verification task retries, then add tasks with governed effects after reconciliation tests pass. Disabling clean retries preserves the action journal, current policy and all prior receipts. Never make rollback delete failed-attempt evidence.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
