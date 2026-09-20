# MR-03: Final-payload context budgets and protected projections

- **State:** In progress; minimal typed projection and retained-item receipt implemented
- **Priority:** P0: context correctness
- **Owner:** Go memory projection, with host/provider assembly and economizer accounting
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md) for eligible candidates
- **Delivery:** Three implementation slices

## Problem and intended result

The Go typed-context projection now emits selected evidence without channel
budget/status diagnostics and renders reviewed procedures only in their dedicated
envelope. The response records exact rendered bytes, a deterministic SHA-256
projection digest and retained channel/ID pairs. Procedure identities use the
specific proposal ID. The legacy bytes/4 estimate remains visible for compatibility
but explicitly has unavailable token-count provenance. A fixed two-item regression
renders the same evidence in 280 bytes instead of 1,044 bytes; this is a projection
size result, not a provider token or latency measurement. Full provider-request
accounting, source-version binding, protected packing and complete channel/host
feedback remain acceptance work.

The shipping Go ingress plan also supplies version-one `context_limits` with
`max_context_bytes`. The assembler verifies the final memory envelope's UTF-8
byte length, reports its digest and exact retained memory IDs, and preserves
literal zero separately from the legacy inherited budget. Unknown limit fields,
unsupported schemas and token caps/reserves without provider counting are
refused. This boundary covers the memory envelope; subsequent provider formatting,
full-request token accounting and protected content still require integration.

Automatic legacy ingress now reports retained code indices and exact clipped
memory previews from Go packing. The host emits this assembly evidence only after
integrity acceptance, preventing omitted or rejected candidates from receiving
exposure feedback. [Boundary validation](../../validation/memory-ingress-evidence-2026-09-20.md)
distinguishes this completed correction from remaining provider dispatch and
all-channel accounting work.

[Provider-bound capture](../../validation/memory-provider-boundary-2026-09-20.md)
found and corrected dropped instructions/tools in buffered Responses and wrong
wire serialization for Chat/Responses targeting Anthropic. The deployment matrix
now checks the complete Go-recalled memory projection, user constraints and tool
schemas at the actual HTTP provider boundary. This validates preservation and
exact request byte accounting in the fixture; production hard caps and tokenizer
integration remain required.

Row-count heuristics and summary-only token estimates do not bound serialized model context. Full JSON items, metadata, wrappers, directives and duplicated procedure text can be larger than the representation charged to the budget. The existing outer ingress byte envelope is a useful backstop, but dropping a complete typed channel after assembly defeats the intended allocation.

Use a single model-facing projection and verify the final request budget after every provider-affecting transformation. Keep explanation metadata outside the prompt unless a small field is explicitly useful to the model.

## Existing integration points

Change `RecallBundle` and `AssembleContext` in `server-go/modules/memory/retrieval.go`. Move memory-specific typed selection/projection from `src/kb/db2_adapters/kb_service_backend_context.c` into the Go memory owner, exposed through the versioned memory-data contract. `ingress_render_block` in `src/server/ingress_preinject.c` and provider adapters retain outer host packing and final request accounting. Reuse the exact-count and provenance requirements in `server-go/modules/economizer`.

The host reports final retained memory IDs/spans and projection identity to Go memory for coverage evaluation after any outer trim or transform. Bind that result to the exact plan revision; do not let C reconstruct memory sufficiency from item counts. The Go projection and host request share explicit budget/count provenance while each owner enforces its own boundary.

## Budget contract

Represent limits with named units: `max_context_bytes`, `max_context_tokens`, `max_request_tokens`, `reserved_response_tokens` and `reserved_tool_tokens`. Values include explicit `count_state` (`exact`, `conservative_estimate`, `unavailable`) and tokenizer/model revision. Zero is a literal zero where allowed; absence requests an inherited limit. Do not reuse a field whose legacy zero means disabled.

An internal `ContextProjection` has stable item IDs, protected class, rendered bytes, source versions and omission reasons. An `AssemblyResult` contains retained IDs, final bytes, token accounting, projection digest and packing trace. Exact provider request accounting includes roles, tool definitions, wrapper text and provider serialization overhead supported by the tokenizer contract.

## Packing behavior

1. Derive a minimal projection from eligible candidates. Render reviewed procedures once while preserving their reviewed status. Keep untrusted evidence separate from authority-bearing instructions.
2. Reserve mandatory policy and explicit user-constraint content. A model-generated label cannot promote an item into a protected class.
3. Select optional evidence under both byte and token limits. Coherent required bundles may be atomic; record why they fail to fit.
4. Serialize the actual request and count again. Remove optional items deterministically and reserialize until the caps hold. Bound iterations and work.
5. If protected content alone exceeds capacity, return `protected_context_overflow`; choose an authorized larger-context route, split the task or ask for a narrower task. Do not silently truncate policy.
6. Record exact final retained IDs. [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) recomputes sufficiency after this step; [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) records assembly and dispatch separately.

No implicit “always include the first result” exception is permitted. Missing exact tokenization cannot support an exact-cost claim. An explicitly supported conservative estimator may permit ordinary serving with its uncertainty recorded; an unknown estimator with no defensible bound must not claim compliance with a hard token cap.

## Protected-content transformation

Preserve user constraints, negation, numerical bounds, deadlines, required identifiers and authority-bearing instructions through any condensation step. Bind transformed bytes to the request/attempt and require existing economizer admission before applying a cost-saving transform. Heuristic commitment extraction is a check, not proof of semantic equivalence. Failed validation falls back only if the original still fits; otherwise return an explicit overflow outcome.

## Implementation slices

1. Introduce the projection/result types and provider-bound counting adapters. Add byte-accounting coverage for every component.
2. Replace bundle row heuristics as a budget enforcement mechanism, migrate typed memory projection into Go and remove duplicate typed-procedure rendering. Return retained/omitted IDs from the outer packer through the shared contract; remove converted C selection policy.
3. Add protected-content validation, full-request recount after economizer/provider adaptation and bounded overflow handling.

## Acceptance gates

- Long metadata cannot be admitted at the cost of its short summary. JSON escapes, Unicode, channel wrappers, reminders, directives and empty-channel overhead are counted.
- Final request bytes/tokens obey declared hard limits; item count is not accepted as a substitute.
- A small required constraint survives a large optional item. Required evidence dropped by packing makes sufficiency partial.
- The model-visible procedure is present once and carries the correct trust classification.
- A transform that breaks negation, a numerical limit or a protected prefix is rejected; a fallback that exceeds capacity is not sent.
- Repeated assembly over identical inputs and versions yields identical projection bytes, selection and digest.

## Rollout and rollback

Run old/new packing in shadow to measure changes in retained evidence, false overflow, task outcomes and latency. Enable by endpoint after its cap tests pass. Preserve the outer byte guard throughout. A ranking rollback must not restore inaccurate accounting or duplicate rendering.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
