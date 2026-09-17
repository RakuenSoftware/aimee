# MR-15: Procedure experience, delayed outcomes and total task cost

- **State:** Proposed
- **Priority:** P1 for reward correctness; P2 for experience projections and tuning
- **Owner:** Learning/outcome attribution and economizer
- **Depends on:** [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md), [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md), [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md); evaluate with [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md)
- **Delivery:** Four implementation slices

## Problem and intended result

Result-count rewards can label a nonempty but irrelevant recall as sufficient. A reviewed procedure may also appear equally reliable after one proposal or many successful applications. Exposure is not application, and application is not verified success.

Extend existing outcome attribution with exact delivered/applied evidence, evidence-linked procedure experience and task-level quality/cost reporting. Preserve the current exact-cost intervention gate.

## Event and projection contract

Reuse `learning_application_events`, current learning/outcome records, the retrieval outcome bridge and KB bandit integration in `src/kb/kb_service_memory.c` and `src/kb/kb_bandit.c`. Join each event to procedure ID/version, task/attempt, context receipt, applicability/environment, relevant tools/actions and verifier outcome.

Separate states: retrieved, delivered, selected-for-use, applied, verified-success, verified-failure, abandoned and outcome-unknown. Only host-observed execution or explicitly attributed user feedback can establish application/outcome. A model claiming it followed a procedure is an unverified signal.

The derived experience envelope exposes application counts, success/failure/unknown counts, last verified success, known counterexamples, observed environments, applicability gaps, task cost/latency and evidence references. Counts are by defined application/trial identity, not number of repeated events or source summaries. Report attempts and terminal task outcomes separately to avoid hiding repair failures or double-counting retries.

Canonical procedure text remains behind existing review gates. An outcome projection can propose a revised applicability condition or counterexample; it cannot silently rewrite instructions.

## Reward and attribution

Retain result count as an availability/truncation feature, with that exact name. Do not use it as the primary sufficiency or correctness reward. Attribute delayed outcomes only to evidence actually delivered and, where known, actually applied.

Use verifier-specific outcomes: tests with bound revisions, externally confirmed effects, reviewed answer support, or explicit user evaluation. Unknown outcomes are not zero-quality failures or automatic successes. Guard against self-reward, duplicate events, cherry-picked successes and a trivial write masquerading as task progress.

Observed success associations are not causal proof that a memory/procedure improved the task. Separate environment/model/task-class cohorts and evaluate policy changes with paired or randomized designs where appropriate. Confidence intervals and sample counts accompany success-rate claims; correlated trials are labeled.

## Cost contract

Record actual and estimated usage separately for indexing/embedding, retrieval/reranking, context transformation, generation, tool calls, retries and verification. Distinguish marginal serving cost from amortized ingestion/background cost, and state the allocation rule. Pin provider/model/pricing snapshot and cache assumptions for every cost computation.

Use integer monetary units and retain existing exact-token/exact-request provenance requirements for intervention admission. A shorter prompt alone does not establish lower cost; cache loss, recovery and verification may reverse the result. Compare paired task quality, completion and total cost, with no unpriced calls silently excluded.

## Implementation slices

1. Rename/separate count/truncation proxies and join delayed feedback to actual delivered evidence; do not fit new weights yet.
2. Add idempotent application/outcome events and versioned procedure experience projections with unknown states.
3. Add complete task-cost attribution and paired reporting while preserving the economizer admission contract.
4. Tune routing/ranking/reward policies only after frozen evaluation and minimum useful feedback coverage; keep the previous policy artifact available.

## Acceptance gates

- One irrelevant hit cannot earn a correctness/sufficiency success merely by being nonempty.
- A retrieved-but-omitted or unused procedure does not acquire a successful application.
- Duplicate events and retries produce the defined counts; conflicting outcome evidence is explicit.
- Changing a procedure version starts the appropriate version cohort without erasing prior history.
- Unknown/unpriced usage is visible; total reported cost includes all declared stages and retries.
- Claimed savings require the specified quality gate and paired baseline; exact-cost admission cannot fall back to character estimates.

## Rollout and rollback

Deploy attribution in observe mode before reward changes. Remove known misleading success semantics independently of new learning. Roll back fitted policies without erasing collected outcome evidence or weakening the existing cost-proof boundary.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
