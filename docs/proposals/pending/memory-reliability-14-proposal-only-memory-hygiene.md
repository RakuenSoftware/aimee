# MR-14: Proposal-only memory hygiene and bounded maintenance

- **State:** Proposed
- **Priority:** P2: operational maintenance
- **Owner:** Go memory maintenance/proposal generation, with the reviewed-learning owner
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-02](memory-reliability-02-authority-preserving-mutations.md), [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md), [MR-08](memory-reliability-08-retrieval-health-telemetry.md), [MR-13](memory-reliability-13-disposable-task-projections.md)
- **Delivery:** Three implementation slices

## Problem and intended result

Long-lived memory accumulates duplicates, contradictions, obsolete assertions, broken correction chains, unsupported observations and influential low-trust records. Automatic model-authored cleanup can destroy history or turn speculation into authoritative facts.

Add a bounded hygiene job that detects problems and creates reviewable proposals. Deterministic derived-state maintenance has a separate allowlist. The new job must not call a broad mutating maintenance mode merely because that mode already exists.

## Existing integration points

Reuse `server-go/modules/memory/maintenance.go`, existing lint/drift functions, `learning_proposals` and current review/commit machinery. Inventory existing automatic promotion, merge, prune and summarize behavior before attaching the hygiene scheduler. The proposal-only guarantee applies to this new path and must be enforced at its mutation boundary.

Implement detectors and memory finding projection in the Go memory owner, reusing its eligibility, lineage and mutation contracts. Scheduling supplies bounded work over the bus; the existing learning owner retains review/commit authority. C maintenance commands remain adapters and cannot call a local mutation path when the Go module is unavailable.

## Job and finding model

Each run has identity, authorized scope, snapshot/watermark, time/row/token/cost limits, detector versions, resume cursor and terminal state. Claim work using existing job primitives; make resume/retry idempotent. Prioritize serving and correction work above optional model-assisted scans.

Finding types include potential duplicate cluster, possible contradiction, obsolete assertion candidate, broken correction chain, missing dependency, unreferenced observation, over-exposed memory, low-trust high fan-out and expired task projection. Each finding includes evidence IDs/versions, reason codes, uncertainty, proposed operation and expected-version preconditions. “Not used recently” is not proof that a memory should be deleted.

Deduplicate proposals by detector/policy plus target versions and proposed action. A rejected proposal does not recur indefinitely with equivalent content; reconsider only on meaningful new evidence or an explicit reviewer request.

## Mutation policy

| Operation | Hygiene authority |
|---|---|
| Delete expired disposable task cache after lease checks | Deterministic maintenance allowed |
| Queue a governed re-embedding/rebuild job | Allowed through [MR-11](memory-reliability-11-embedding-generations-and-index-freshness.md)'s existing admission |
| Recompute metrics/derived freshness | Allowed without canonical content changes |
| Merge/deduplicate canonical memories | Proposal and existing review required |
| Supersede assertions or rewrite summaries | Proposal and source-version review required |
| Promote a procedure, alter authority or delete durable memory | Existing explicit admission/review required |

Model output supplies candidate findings, never executable SQL or privileged operations. The host translates accepted proposal types into narrow commands. Review binds the exact change preview and current expected versions. If inputs changed after review, recompute/review; do not silently apply a different mutation.

## Proposed interface

`aimee memory hygiene --scope <authorized-scope> --dry-run --json` reports coverage, partial status, findings and budgets. A normal run creates proposals but has no generic auto-apply switch. Review/commit occurs through the existing governed proposal interface. “Dry run” performs no canonical writes or proposal creation; optional run telemetry is identified separately.

The scheduler is opt-in initially. Model-assisted detectors use an allowed model route and explicit budget. An exhausted run returns partial coverage with a resume token, not a clean bill of health.

## Implementation slices

1. Implement deterministic detectors over current indexes/dependencies and read-only preview output.
2. Add proposal generation, version-bound review and rejection deduplication. Enforce narrow worker privileges so proposal generation cannot directly merge/promote/delete canonical content.
3. Add opt-in scheduling, resumable bounded work and optional model-assisted contradiction/obsolescence suggestions. Measure reviewer usefulness and false positives.

## Acceptance gates

- Malicious model output cannot directly mutate canonical memory or expand scope.
- Rejected/expired hygiene proposals leave canonical state unchanged.
- Identical retries do not duplicate proposals; concurrent canonical changes invalidate stale previews.
- Expired projection cleanup leaves source records and action receipts intact and respects active leases/retention.
- A partial run exposes what was not scanned; it does not report zero problems for unvisited data.
- Scoped runs and reports cannot discover unauthorized records through cluster counts or samples.

## Rollout and rollback

Start manually with dry runs and review acceptance statistics. Enable scheduling only with bounded workload and queue observability. Disable scheduling/detectors independently; preserve reviewed proposal history. Do not roll back committed canonical changes by deleting audit records.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
