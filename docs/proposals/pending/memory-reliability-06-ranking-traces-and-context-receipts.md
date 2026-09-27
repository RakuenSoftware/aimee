# MR-06: Actual ranking traces, context receipts and evidence states

- **State:** Complete — implementation and acceptance validated 2026-09-25
- **Priority:** P0 for receipt correctness; P1 for complete diagnostics
- **Owner:** Go memory traces, with host/provider dispatch and audit receipts
- **Depends on:** [MR-03](memory-reliability-03-final-payload-context-budgets.md); joins [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) and [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) outputs
- **Delivery:** Four implementation slices

## Problem and intended result

Post-hoc diagnostic scores need not explain actual SQL/dense/graph ordering. A retrieval event emitted before final packing can include records the model never received. A digest proves correspondence when bytes are available; it cannot reconstruct missing content by itself.

Legacy automatic ingress now records only Go-retained ordinary memory/code items
after host integrity acceptance. Outcome feedback receives the exact clipped
memory preview. Failed assembly and rejected/empty envelopes emit no such evidence.
[Validation](../../validation/memory-ingress-evidence-2026-09-20.md) covers these
boundaries; complete channel coverage, durable stage receipts, dispatch observation
and crash recovery remain open.

The [typed outer assembler](../../validation/memory-typed-outer-packing-2026-09-20.md)
now returns retained typed IDs and source/final projection and selection digests
after row-level repacking. This is an assembly response before host integrity
acceptance. The host now also [emits Go-retained typed projection references](../../validation/memory-typed-assembly-evidence-2026-09-20.md)
after integrity acceptance, preserving final selection identity without inventing
source versions. The [plain-text facts block](../../validation/memory-fact-source-versions-2026-09-21.md)
now also binds exact assertion and direct-parent revisions, validates their byte
commitments in Go assembly, and emits only retained references after host integrity
acceptance. The durable host/provider receipt pipeline and final source revalidation
remain open. Bounded trace reads now refuse partial payloads.

Record distinct retrieval, selection, assembly, preparation, dispatch and acknowledgement stages. Connect them to the existing audit/WORM infrastructure with bounded metadata and truthful evidence states.

## Trace contract

Carry a request-scoped trace object through the call chain. Avoid process-global mutable trace buffers: concurrent requests and nested recalls must not mix evidence.

Each candidate includes stable ID/version, arm ranks and native score semantics, actual fusion contribution, prior effects, eligibility decision, source-family projection and final disposition. Dispositions include selected, duplicate, insufficient relevance, scope/lifecycle/policy exclusion, type/coverage displacement, caller limit, budget drop and unavailable evidence. Record the candidate-universe bounds and trace truncation; a bounded trace is not proof that the entire database was searched.

`memory explain` reads this trace. It must not reconstruct lexical/dense scores from content substring checks after the search.

The [observed diagnostic ranking slice](../../validation/memory-program-gates-2026-09-23.md)
captures the actual deduplicated RRF arm ranks/contributions, candidate-order
resets, negation boost and optional PageRank addition for returned candidates.
Each stage describes its resulting score; prior stage scores are not summed as
extra votes. Public diagnostic parts expose the steps, and existing trace
feature JSON retains their numeric evidence. Ordinary recall and automatic
preview packing do not enable diagnostic capture or change their score contract.

This remains a bounded returned-candidate trace, not the full candidate universe.
Native lexical/dense scores, excluded-candidate dispositions, source versions and
durable provider stages are not certified by these rank observations. Exact-ID
explain_match text matching is explicitly labeled text_match_estimate; it does
not pretend to replay a retrieval decision.

## Receipt contract

The host-owned receipt includes request/task/turn/attempt IDs, principal/scope reference, policy versions, requirement and contract revisions, index/embedding identities, selected source versions/spans, projection/renderer version, final model-input payload digest, byte/token counts and count provenance.

Use a versioned canonical metadata encoding and a separate digest of the exact final request body consumed by the provider adapter. Do not hash credential headers into a published receipt. Finish all provider/economizer transformations before binding final bytes; bind each changed retry body separately.

| Stage | Meaning |
|---|---|
| `retrieved` | Candidate collection completed or explicitly degraded |
| `assembled` | Final projection and packing decisions exist |
| `prepared` | Exact request binding durably appended to the governed audit/WORM pipeline before network dispatch |
| `dispatch_admitted` | Durable intent for one transport attempt; bytes may or may not have been sent |
| `dispatch_started` | A concrete transport attempt began |
| `acknowledged` | Provider returned an identifiable acknowledgement/response |
| `failed` / `outcome_unknown` | Failure is known, or transport uncertainty prevents claiming a result |

The prepared stage must survive a crash. If the required durable append is unavailable, governed dispatch waits/fails; it does not proceed with an in-memory promise to log later. Existing transactional outbox acceptance may serve as that durable boundary if its durability contract is explicit. Local acceptance, chain sealing and external checkpoint delivery remain separate statuses. Optional sampled health telemetry is never the sole required receipt.

Persist `dispatch_admitted` before handing bytes to the transport, with one host-owned attempt identity and dispatch ownership. Recovery may call an attempt unsent only when it remained `prepared` without admission and no dispatcher can still admit it. After admission, a missing `dispatch_started` or acknowledgement record does not prove that no bytes were sent. Record `outcome_unknown` unless independent transport/provider evidence resolves the attempt. A crash after receipt of a response but before its durable acknowledgement has the same uncertainty rule.

Transport handoff and network effects are not atomic with the receipt store. Do not rename durable intent as observed dispatch to hide that gap. Retry decisions preserve the unresolved attempt, allocate a distinct attempt identity and account for potentially executed work under the existing budget policy. Governed external effects additionally follow [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md)'s reconciliation/idempotency rules.

## Verification and retention

The [provider handoff source check](../../validation/memory-source-revalidation-2026-09-21.md)
now revalidates retained assertion/episode and direct-parent versions under the
authenticated KB scope. Its process-local handles and attempt challenges refuse
stale or unavailable sources; they are not durable prepared/dispatch receipts and
do not bind the final body or eliminate mutations after the check's snapshot.

Expose independent evidence dimensions: schema-valid, authenticated producer, source-version available, payload-verifiable, decision-replayed, chain-included, externally-compared and effect-confirmed. Do not collapse them into a single “verified” bit or assume an ordering of strength.

Support two explicit retention modes. `commitment_only` allows verification against separately supplied bytes and may retain source/version references; it does not promise reconstruction. `replayable` requires all inputs needed for deterministic reconstruction or a governed encrypted final payload, with access/retention controls. If a dependency is erased or unavailable, report replay unavailable while preserving any valid chain inclusion result.

## Existing integration points and slices

1. Extend `recall_traces`/`recall_trace_results` and request-scoped Go memory tracing with actual ranking and memory projection contributions. Return bounded versioned trace references through the bus; remove retrospective C reconstruction for migrated paths.
2. Make `ingress_render_block` return retained IDs and dispositions; emit final assembly evidence after packing for ordinary memory, code, facts, observations and procedures.
3. Add durable preparation and dispatch admission at the final provider-request boundary, followed by idempotent observed dispatch/acknowledgement events and crash recovery. Reuse existing audit/WORM writers.
4. Add `aimee memory receipt <request-id> --json` and receipt verification with explicit retained-input/evidence states.

## Acceptance gates

- A tiny envelope distinguishes retrieved-but-omitted items from delivered items across every channel.
- Dense-only and graph-promoted results show the actual contribution that affected ordering.
- Concurrent/nested calls cannot contaminate trace IDs or candidate lists.
- Crash after preparation but before durable dispatch admission yields prepared-without-dispatch once dispatch ownership is resolved, never a successful invocation.
- Crash after admission but before handoff, after send but before dispatch logging, or after response receipt but before acknowledgement persistence preserves uncertainty unless independent evidence resolves it. Absence of a log record never proves non-dispatch after admission.
- Timeout after send records uncertainty and does not invent a provider acknowledgement.
- A changed request body, source version, renderer or policy cannot verify against the prior binding.
- Commitment-only receipts cannot claim replay; deletion of replay inputs does not erase the distinction between replay and inclusion.

## Rollout and rollback

Add trace fields compatibly, then switch final-selection events and governed preparation by surface. Cap candidate metadata and measure overhead. Preserve required durable receipts on rollback; disable optional detailed diagnostics separately.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)

## Final acceptance — 2026-09-25

All eight acceptance clauses are validated in the
[MR-06 closeout and evidence](../../validation/memory-mr06-closeout-2026-09-25.md).
Earlier slice notes above preserve their historical scope. The final closeout
records the completed work and explicit unavailable-evidence/retention bounds.
