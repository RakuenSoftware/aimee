# MR-10: Deterministic utility horizons for transient memory

- **State:** Proposed
- **Priority:** P1: after shared validity enforcement
- **Owner:** Memory lifecycle policy
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md); measure with [MR-08](memory-reliability-08-retrieval-health-telemetry.md)/[MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md)
- **Delivery:** Three implementation slices

## Problem and intended result

Temporary task state or an old debugging failure can stay semantically similar long after it stops being useful for ordinary current-state context. Score decay cannot express a clear exclusion policy. Conversely, age alone does not invalidate a durable architectural decision or user constraint.

Add a deterministic, purpose-specific utility horizon separate from valid time, retention and deletion. An elapsed horizon excludes the record from ordinary automatic serving under that policy; it does not assert the content is false or erase it.

## Policy and precedence

Extend the existing lifecycle owner with `utility_horizon_policy_version`, an authenticated anchor event and an optional explicit ordinary-serving deadline. Resolve the effective policy from system safety rules, authorized per-record override, domain policy and record-kind default. Record which rule won. A model cannot declare its own record permanent.

The initial production policy applies only to explicitly classified transient/task state with a configured duration. Other kinds have no new age exclusion until measurement and operator policy justify it. Reviewed procedures, durable constraints and preferences are not assigned a universal expiration merely by this feature; they remain subject to ordinary validity, evidence and review rules.

Use creation or authenticated last-confirmation time according to policy. Serving, reading or incrementing `use_count` never extends the horizon. A confirmed update creates a new version/anchor through [MR-02](memory-reliability-02-authority-preserving-mutations.md). Do not infer confirmation from an agent mentioning the memory again.

| Operation | Effect of elapsed utility horizon |
|---|---|
| Ordinary current context | Exclude with `utility_horizon_elapsed` |
| Authorized historical/as-of request | May return with temporal/horizon labels |
| Explicit diagnostic search | May return with visible serving restriction |
| Canonical deletion or revocation | Still enforced regardless of query mode |
| Durable audit/history | Governed by its own retention policy |

A historical/diagnostic override bypasses only the utility-horizon filter. It cannot bypass authorization, erasure, rejection or quarantine, and it cannot relabel an old record as current truth.

## Implementation

Implement the horizon evaluator as a pure function of record version, authenticated anchor, request purpose/time and policy artifact. Integrate it through [MR-01](memory-reliability-01-unified-eligibility-and-validity.md) instead of adding per-endpoint age SQL. Handle absent/malformed anchors explicitly: return unknown policy applicability and use the declared conservative serving rule; do not substitute current time.

Expose the decision in `memory validity`, actual selection traces and health reports. Distinguish `utility_horizon_elapsed`, domain validity expiry and physical retention deletion. A policy change creates a new versioned decision and invalidates affected cached projections without rewriting historical records.

## Implementation slices

1. Add evaluator, policy schema, auditability and boundary-time fixtures with the feature in shadow mode.
2. Enable explicitly transient categories and authenticated record overrides; integrate all recall and task-projection consumers.
3. Tune domain/kind policies using historical-task preservation and task-outcome data. Learned horizon changes remain future reviewed policy proposals.

## Acceptance gates

- An expired transient state does not enter ordinary context through lexical, dense, graph, bundle or cache paths.
- Authorized historical recall still returns the applicable version; deleted/revoked content remains withheld.
- Repeated serving cannot renew a horizon. Only the declared confirmation/update operation can change its anchor.
- Exact boundary, missing anchor, clock skew, policy revision and override precedence have deterministic results.
- Durable constraints/decisions do not disappear due to an unconfigured catch-all duration.

## Rollout and rollback

Start with observed would-exclude counts, query classes and false exclusions. Enable per domain/kind after its fixture and quality gates pass. Disabling the horizon restores ordinary horizon eligibility, not records erased or prohibited by other controls. Do not use horizon expiry as an implicit housekeeping delete.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
