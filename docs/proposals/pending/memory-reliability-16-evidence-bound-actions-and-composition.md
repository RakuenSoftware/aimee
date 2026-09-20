# MR-16: Evidence-bound actions, idempotent receipts and composition policy

- **State:** Proposed
- **Priority:** P1 where memory informs external effects; composition extensions follow separately
- **Owner:** Execution policy, action runtime and audit
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-02](memory-reliability-02-authority-preserving-mutations.md), [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md), [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md)
- **Delivery:** Four implementation slices

## Problem and intended result

A memory can be valid at recall time but revoked or corrected before it is used for an external action. An old allow decision or a matching content hash does not authorize a different destination. Individually allowed actions can also form a prohibited sequence when combined across turns or delegates.

Bind governed actions to current evidence, exact request identity and host-owned policy; record precise attempt/outcome states. Extend the existing policy engine for bounded composition checks instead of introducing a second authorization authority.

## Action intent and admission

Create a host-issued intent with action/request/task/attempt IDs, authenticated principal, trusted tool/endpoint class, exact destination/object identity, request-body digest, purpose, selected evidence versions, context receipt, policy/revocation generations, expiry and idempotency key.

The tool registry supplies effect class and resource semantics. Agent text cannot label a publish operation as a harmless read. Existing operator and tool authorization always applies; possession of a context receipt grants no additional access.

Obtain memory source-version and freshness decisions through the Go memory contract. Execution policy consumes that evidence and owns action admission; it must not recreate memory eligibility in the host or grant memory the authority to dispatch tools. Missing required memory-owner evidence blocks admission under the freshness rule below.

Immediately before durable dispatch admission, revalidate required source versions, current authority, destination and policy. Define the admission linearization point: the transaction or equivalent atomic host decision that commits this exact intent for dispatch. A revocation committed before that point must block admission or force a new plan. A revocation after an external effect has started cannot be promised to undo it; attempt cancellation/reconciliation and record the ordering honestly.

When relevant generations span owners, require current checks or bounded, authenticated freshness leases under explicit policy. Do not claim a cross-system atomic snapshot without implementing it. Missing required freshness blocks the governed action.

## Idempotency and outcome state

States distinguish prepared, admitted, dispatching, acknowledged, effect-confirmed, failed-before-effect and outcome-unknown. Bind a repeated idempotency key to actor, action class, destination and exact approved payload; changed material receives a conflict. A matching body sent to another external object is a different action.

Use provider idempotency facilities when available and retain host state across restarts. If a timeout leaves an irreversible action's outcome unknown and the provider lacks safe idempotent replay, reconcile the destination before retry or request an explicit decision. Do not blindly repeat it.

Claims such as “saved”, “sent” or “completed” must be generated from the applicable host receipt. An API acknowledgement and a verified external effect remain different evidence levels. Verification checks the intended object, not any object with equal content.

## Composition checks

Extend existing task/action lineage with trusted action classes, destinations, sensitivity classes and accumulated cost/work. Policies may prohibit combinations, ordered sequences or aggregate budgets. Examples include sensitive read followed by external publication, permission elevation followed by use, and many small calls exceeding a task ceiling.

Evaluate cumulative state atomically with admission/reservation. Parallel delegates and task forks inherit the relevant lineage; splitting a workflow cannot reset the policy. Return reason codes and the authorized remediation path without exposing hidden sensitive history to an unprivileged actor.

## Implementation slices

1. Add evidence-bound intents and a freshness recheck at the existing execution-policy/action dispatch boundary, including exact destinations.
2. Persist idempotency and crash-safe outcome states; bind user-facing completion claims to receipts.
3. Add cumulative resource reservations and a narrow set of trusted composition rules in the current policy engine.
4. Add reconciliation tools and tests for unknown outcomes, cancellation and cross-owner freshness; measure admission overhead.

## Acceptance gates

- Revocation/correction before admission blocks stale evidence use; concurrent ordering is reproducible in audit events.
- Old permits, changed payloads, changed destinations and forged tool classes cannot authorize execution.
- Concurrent duplicate submissions create at most the effect allowed by the provider/host idempotency contract.
- Unknown irreversible outcomes are not silently retried or reported as success.
- Splitting a prohibited sequence across turns, delegates or retries cannot bypass composition/spend rules.
- Failed external-effect verification cannot be presented as confirmed completion.

## Rollout and rollback

Start with a small set of external-write tools whose result semantics are well defined. Composition analysis may run in shadow initially; required authorization/idempotency fixes do not. Rollback can remove an optional composition experiment while preserving admission freshness and receipt truthfulness.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
