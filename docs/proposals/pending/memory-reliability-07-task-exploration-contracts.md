# MR-07: Task-bound exploration contracts with starvation recovery

- **State:** Proposed
- **Priority:** P1: enable enforcement only after context correctness gates
- **Owner:** Task runtime, attention guard and execution policy
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-03](memory-reliability-03-final-payload-context-budgets.md), [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md), [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md); evaluate under [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md)
- **Delivery:** Four implementation slices

## Problem and intended result

An agent can ignore useful indexed context and spend repeated turns rediscovering the repository. Restricting exploration using an uncalibrated confidence score creates the opposite failure: wrong context prevents the reads needed to correct it.

Introduce a host-issued task contract that prefers supplied/indexed context, limits redundant supplementary discovery and has an explicit escape path. It controls exploration efficiency, not permission to access data or perform actions.

## Existing integration points

Reuse `src/cli_attention_guard.c`, `src/headers/cli_attention_guard.h`, `src/server/execution_policy_bus.c` and `server-go/modules/execution-policy/execution_policy.go`. The current raw-scan setting treats values less than or equal to zero as disabled; a new contract budget of zero must mean zero permitted scans. Preserve legacy configuration meaning through an explicit adapter.

The attention guard and execution-policy source-discovery check must consume the same decision. An indexed-search failure escape cannot work if a separate guard unconditionally blocks the fallback. Bind a fallback capability to the host-recorded failed/empty indexed-tool attempt, permitted discovery class/path scope, expiry and remaining allowance; consume it through authenticated request context, never an agent-supplied bypass boolean. Do not claim starvation recovery until both paths are covered.

## Contract and limits

Persist a host-owned contract in existing task/session state. A local read-only reference or protected cache may expose it to hooks; the agent cannot author the authoritative file. Include contract ID/revision, task/session/principal, project/worktree generation, plan/receipt digest, requirement coverage, confidence provenance, supported tool classes, creation/expiry time and supplementary limits.

Consume coverage, index freshness and recovery-gap results from the Go memory contract. Task runtime owns contract issuance and execution policy owns enforcement; neither duplicates memory eligibility or sufficiency in a C hook or tool wrapper. Bind each consumed result to its memory plan/source versions so a module restart or incompatible result forces refresh or observe mode under baseline policy.

Use typed limits: `enabled: false` disables the adaptive contract; a missing ceiling inherits operator policy; an explicit nonnegative number is a literal allowance. Normalize an absent operator ceiling to unbounded internally, not integer zero. Effective allowance is the minimum of remaining operator allowance and remaining contract allowance. Keep units and reset scopes separate: a session ceiling is not reset when a task revision changes.

Budgets may cover raw scan invocations, distinct supplementary files, graph expansions and returned bytes/tokens. Reopening one file is not a new distinct file, but still consumes applicable read-byte/operation budgets. Recommended-file reads and indexed expansions remain subject to operator access and total work ceilings.

## Enforcement and recovery

Default mode is `observe`. Enforcement requires operator opt-in and a supported query class with requirement-complete context, a fresh index/contract and measured confidence calibration. A high similarity score alone is insufficient. Initial policy can redirect raw scans to indexed tools; it must not remove the ability to inspect whether supplied evidence is wrong.

The host classifies tool operations. Apply the new budget to recognized pure discovery calls, including registered aliases. Do not automatically rewrite shell commands. Compound shell commands that mix reads and writes remain under the existing execution policy; v1 must not block a legitimate edit merely because a heuristic finds a search substring. This performance control is not a substitute for a sandbox or authorization boundary.

On a restriction, return a structured reason and specific available alternatives: indexed symbol lookup, code graph search, named span expansion or `context_contract_expand`.

For expansion, the agent supplies a reason and gap reference. The host verifies recent tool outcomes and issues a new revision, preserving the old one. A configurable starvation threshold (initially two completed constrained turns) may relax only the adaptive budget. “No progress” is derived from unresolved requirement gaps, failed/empty indexed lookups and task verification results; a trivial write or a model saying “done” cannot reset it. Lower the enforcement tier without rewriting a statistical confidence value.

Expired, malformed, wrong-task or wrong-project contracts are ignored as adaptive controls and logged; baseline operator/access policy still applies. Issuer unavailability falls back to operator policy. If baseline policy itself prevents further exploration, stop with the specific unresolved gap or request an operator decision. Automatic relaxation never increases access.

## Implementation slices

1. Add typed limits, authenticated contract state and atomic budget reservation/refund semantics. Concurrent delegates share the applicable task allowance.
2. Emit observe-only decisions from the final context plan. Unify attention-guard and execution-policy handling, including indexed-empty fallback.
3. Add explicit expansion, host-observed starvation recovery and contract invalidation on task/scope/index changes.
4. Enable enforcement for one measured workload and expand only after [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) efficiency/noninferiority gates pass.

## Acceptance gates

- Literal zero scans and legacy disabled-zero configuration remain distinguishable.
- Malformed, expired and forged contracts cannot widen operator permission or transfer limits to another task.
- Concurrent substeps cannot overspend a shared allowance; retrying a tool dispatch cannot double-charge the same admitted attempt.
- Deliberately wrong “high-confidence” context leads to bounded indexed recovery/contract expansion without deadlock.
- Disallowed files and actions remain disallowed after starvation relaxation.
- Edits and unrelated safe operations are unaffected by a discovery-only cap.
- Paired tasks show fewer redundant raw scans and no unacceptable loss of completion, correctness or latency under predeclared [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) gates. Report false-restriction and expansion rates.

## Rollback

Return adaptive mode to observe while preserving baseline policy, receipts and budget history. Never ask the agent to alter operator configuration as the normal recovery path.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
