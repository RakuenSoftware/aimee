# MR-06: Actual ranking traces, context receipts and evidence states

- **State:** Proposed
- **Priority:** P0 for receipt correctness; P1 for complete diagnostics
- **Owner:** Retrieval, ingress/provider adapter and audit
- **Depends on:** [MR-03](memory-reliability-03-final-payload-context-budgets.md); joins [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) and [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) outputs
- **Delivery:** Four implementation slices

## Problem and intended result

Post-hoc diagnostic scores need not explain actual SQL/dense/graph ordering. A retrieval event emitted before final packing can include records the model never received. A digest proves correspondence when bytes are available; it cannot reconstruct missing content by itself.

Record distinct retrieval, selection, assembly, preparation, dispatch and acknowledgement stages. Connect them to the existing audit/WORM infrastructure with bounded metadata and truthful evidence states.

## Trace contract

Carry a request-scoped trace object through the call chain. Avoid process-global mutable trace buffers: concurrent requests and nested recalls must not mix evidence.

Each candidate includes stable ID/version, arm ranks and native score semantics, actual fusion contribution, prior effects, eligibility decision, source-family projection and final disposition. Dispositions include selected, duplicate, insufficient relevance, scope/lifecycle/policy exclusion, type/coverage displacement, caller limit, budget drop and unavailable evidence. Record the candidate-universe bounds and trace truncation; a bounded trace is not proof that the entire database was searched.

`memory explain` reads this trace. It must not reconstruct lexical/dense scores from content substring checks after the search.

## Receipt contract

The host-owned receipt includes request/task/turn/attempt IDs, principal/scope reference, policy versions, requirement and contract revisions, index/embedding identities, selected source versions/spans, projection/renderer version, final model-input payload digest, byte/token counts and count provenance.

Use a versioned canonical metadata encoding and a separate digest of the exact final request body consumed by the provider adapter. Do not hash credential headers into a published receipt. Finish all provider/economizer transformations before binding final bytes; bind each changed retry body separately.

| Stage | Meaning |
|---|---|
| `retrieved` | Candidate collection completed or explicitly degraded |
| `assembled` | Final projection and packing decisions exist |
| `prepared` | Exact request binding durably appended to the governed audit/WORM pipeline before network dispatch |
| `dispatch_started` | A concrete transport attempt began |
| `acknowledged` | Provider returned an identifiable acknowledgement/response |
| `failed` / `outcome_unknown` | Failure is known, or transport uncertainty prevents claiming a result |

The prepared stage must survive a crash. If the required durable append is unavailable, governed dispatch waits/fails; it does not proceed with an in-memory promise to log later. Existing transactional outbox acceptance may serve as that durable boundary if its durability contract is explicit. Local acceptance, chain sealing and external checkpoint delivery remain separate statuses. Optional sampled health telemetry is never the sole required receipt.

## Verification and retention

Expose independent evidence dimensions: schema-valid, authenticated producer, source-version available, payload-verifiable, decision-replayed, chain-included, externally-compared and effect-confirmed. Do not collapse them into a single “verified” bit or assume an ordering of strength.

Support two explicit retention modes. `commitment_only` allows verification against separately supplied bytes and may retain source/version references; it does not promise reconstruction. `replayable` requires all inputs needed for deterministic reconstruction or a governed encrypted final payload, with access/retention controls. If a dependency is erased or unavailable, report replay unavailable while preserving any valid chain inclusion result.

## Existing integration points and slices

1. Extend `recall_traces`/`recall_trace_results`, request-scoped Go tracing and typed packing traces with actual ranking contributions.
2. Make `ingress_render_block` return retained IDs and dispositions; emit final assembly evidence after packing for ordinary memory, code, facts, observations and procedures.
3. Add a durable pre-dispatch receipt at the final provider-request boundary and append dispatch/acknowledgement events idempotently. Reuse existing audit/WORM writers.
4. Add `aimee memory receipt <request-id> --json` and receipt verification with explicit retained-input/evidence states.

## Acceptance gates

- A tiny envelope distinguishes retrieved-but-omitted items from delivered items across every channel.
- Dense-only and graph-promoted results show the actual contribution that affected ordering.
- Concurrent/nested calls cannot contaminate trace IDs or candidate lists.
- Crash after preparation but before send yields prepared-without-dispatch, never a successful invocation.
- Timeout after send records uncertainty and does not invent a provider acknowledgement.
- A changed request body, source version, renderer or policy cannot verify against the prior binding.
- Commitment-only receipts cannot claim replay; deletion of replay inputs does not erase the distinction between replay and inclusion.

## Rollout and rollback

Add trace fields compatibly, then switch final-selection events and governed preparation by surface. Cap candidate metadata and measure overhead. Preserve required durable receipts on rollback; disable optional detailed diagnostics separately.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
