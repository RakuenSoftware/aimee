# Proposal rejection audit — 2026-08-15

This follow-up audits every proposal-rejection PR created during the 2026-08-13 through
2026-08-15 pending-proposal validation run. It corrects the rule used by those PRs:

> A stale plan that names C or SQL seams is not rejected merely because product policy must now be
> implemented in Go. Preserve a still-valid objective, assign its canonical decisions to the
> appropriate Go module, and leave legacy C/SQL code as mechanical adapters, migrations, or
> deletion work. Reject only an invalid, abandoned, redundant/already-satisfied, or inherently
> non-Go objective.

The classification was independently approved by Aimee review
`roundtable-4a0397ffa1bee0da15f5f425`. The machine manifest continues to describe the files that
exist on `testing`; each misrejected row changes back to pending only in that proposal's own
corrective PR. This keeps one atomic proposal per PR without hiding the remaining correction queue.

| Rejection PR | Proposal | Audit verdict | Reason | Corrective tracking |
| --- | --- | --- | --- | --- |
| #2634 | [`delegate-limit-diagnostics-residual.md`](pending/delegate-limit-diagnostics-residual.md) | **Misrejected** | Grouped dispatch is Go-owned, and provider-independent limit-exhaustion diagnostics remain a valid executable requirement. | Corrective PR [#2699](https://github.com/RakuenSoftware/aimee/pull/2699). |
| #2655 | [`config-field-descriptor-save-residual.md`](rejected/config-field-descriptor-save-residual.md) | **Misrejected** | Descriptor-owned save semantics and mechanical coverage remain valid; C serialization is a compatibility baseline, not a reason to discard the objective. | Separate PR required: make Go config authority own the projection and retire adapters after parity. |
| #2657 | [`event-bus-third-language-conformance.md`](rejected/event-bus-third-language-conformance.md) | **Appropriately rejected** | Its defining acceptance criterion requires a client in a language other than C or Go and forbids using either reference implementation. A Go rewrite cannot truthfully satisfy independent third-language evidence. | None; rejection stands. |
| #2660 | [`eval-temp-store-schema-relocation.md`](rejected/eval-temp-store-schema-relocation.md) | **Misrejected** | The objective explicitly permits making the store openable or retiring it; a Go-owned disposable eval-store contract can resolve the live C/SQL conflict. | Separate PR required: rewrite for a Go harness or migrate the last consumer and retire the legacy store. |
| #2662 | [`dataset-benchmark-direct-track.md`](rejected/dataset-benchmark-direct-track.md) | **Misrejected** | Deterministic non-LLM LoCoMo/LongMemEval retrieval measurement remains useful without restoring the retired C binary. | Separate PR required: specify a Go benchmark runner, disposable corpus store, and structured output. |
| #2678 | [`per-user-content-scope-visibility.md`](rejected/per-user-content-scope-visibility.md) | **Misrejected** | The cross-tenant read hole is a live security objective. SQL RLS and workspace/database paths can mechanically apply a Go-owned visibility decision. | Separate PR required: restore pending with Go actor/project binding, fail-closed policy, and migration authority. |
| #2686 | [`capability-scoped-agent-execution.md`](rejected/capability-scoped-agent-execution.md) | **Misrejected** | Resolve-once disclosure/execution parity remains a valid authorization invariant, and delegate execution now has a Go owner. | Separate PR required: rewrite around `server-go/modules/delegates` and an immutable effective capability set. |
| #2688 | [`persona-authored-outputs-residual.md`](rejected/persona-authored-outputs-residual.md) | **Misrejected** | Permission, voice, actor/persona provenance, compatibility, and denial/impersonation evidence remain coherent Go-owned work. | Separate PR required: restore pending and slice Go authorization from mechanical legacy composition/output adapters. |
| #2690 | [`runtime-control-product-boundary-residual.md`](rejected/runtime-control-product-boundary-residual.md) | **Misrejected** | Separate Runtime/Control Go processes, truthful omit behavior, effective config, transport, packaging, and upgrade evidence remain the intended product boundary. | Separate PR required: rewrite around the existing Go process owners and treat packaging/adapters as delivery mechanics. |
| #2692 | [`proposal-evidence-provenance-tiers.md`](pending/proposal-evidence-provenance-tiers.md) | **Misrejected** | The anti-poisoning objective is live; memory policy belongs in Go while existing C/SQL seams transport, persist, migrate, and fail closed. | Corrective PR [#2694](https://github.com/RakuenSoftware/aimee/pull/2694). |

This audit is a lifecycle verdict, not completion evidence. The nine misrejected proposals remain
unresolved until their rewritten acceptance criteria are implemented and pass. The corrective PR
sequence must update this table's tracking text as PR numbers become available.
