# Proposal rejection audit: 2026-08-15

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

The [governance/evidence-provenance proposal](pending/proposal-evidence-provenance-tiers.md) that
triggered this audit is rejection #2692. It was restored under Go memory-policy ownership by
corrective PR #2694. Corrective PRs #2699, #2702, #2704, and #2715 subsequently restored #2634,
#2655, #2660, and #2662. This PR sequence is intentionally atomic: #2678 is the current correction,
and #2686, #2688, and #2690 remain explicitly queued below rather than being silently left rejected
or bundled into an unrelated rewrite.

| Rejection PR | Proposal | Audit verdict | Reason | Corrective tracking |
| --- | --- | --- | --- | --- |
| #2634 | [`delegate-limit-diagnostics-residual.md`](done/delegate-limit-diagnostics-residual.md) | **Misrejected; now implemented** | Grouped dispatch is Go-owned, and provider-independent limit-exhaustion diagnostics remain a valid executable requirement. | Corrective PR [#2699](https://github.com/RakuenSoftware/aimee/pull/2699); Go implementation completed 2026-08-16. |
| #2655 | [`config-field-descriptor-save-residual.md`](pending/config-field-descriptor-save-residual.md) | **Misrejected** | Descriptor-owned save semantics and mechanical coverage remain valid; C serialization is a compatibility baseline, not a reason to discard the objective. | Corrective PR [#2702](https://github.com/RakuenSoftware/aimee/pull/2702). |
| #2657 | [`event-bus-third-language-conformance.md`](rejected/event-bus-third-language-conformance.md) | **Appropriately rejected** | Its defining acceptance criterion requires a client in a language other than C or Go and forbids using either reference implementation. A Go rewrite cannot truthfully satisfy independent third-language evidence. | None; rejection stands. |
| #2660 | [`eval-temp-store-schema-relocation.md`](pending/eval-temp-store-schema-relocation.md) | **Misrejected** | The objective explicitly permits making the store openable or retiring it; a Go-owned disposable eval-store contract can resolve the live C/SQL conflict. | Corrective PR [#2704](https://github.com/RakuenSoftware/aimee/pull/2704). |
| #2662 | [`dataset-benchmark-direct-track.md`](pending/dataset-benchmark-direct-track.md) | **Misrejected** | Deterministic non-LLM LoCoMo/LongMemEval retrieval measurement remains useful without restoring the retired C binary. | Corrective PR [#2715](https://github.com/RakuenSoftware/aimee/pull/2715): restore a Go benchmark runner over the pending disposable memory boundary with structured, baseline-safe results. |
| #2678 | [`per-user-content-scope-visibility.md`](pending/per-user-content-scope-visibility.md) | **Misrejected** | The cross-tenant read hole is a live security objective. SQL RLS and workspace/database paths can mechanically apply a Go-owned visibility decision. | Corrective PR [#2721](https://github.com/RakuenSoftware/aimee/pull/2721): restored under Go `execution-policy`, workspace, and DB2 ownership with fail-closed exact binding and migration authority. |
| #2686 | [`capability-scoped-agent-execution.md`](pending/capability-scoped-agent-execution.md) | **Misrejected** | Resolve-once disclosure/execution parity remains a valid authorization invariant, and delegate execution now has a Go owner. | Corrective PR [#2723](https://github.com/RakuenSoftware/aimee/pull/2723): restored around Go `delegates`, a single immutable effective capability set, and final Go `execution-policy` authorization. |
| #2688 | [`persona-authored-outputs-residual.md`](pending/persona-authored-outputs-residual.md) | **Misrejected** | Permission, voice, actor/persona provenance, compatibility, and denial/impersonation evidence remain coherent Go-owned work. | Corrective PR [#2724](https://github.com/RakuenSoftware/aimee/pull/2724): restored around Go `response-composition`, immutable Go delegate authorship evidence, and separately authorized effect adapters. |
| #2690 | [`runtime-control-product-boundary-residual.md`](pending/runtime-control-product-boundary-residual.md) | **Misrejected** | Separate Runtime/Control Go processes, truthful omit behavior, effective config, transport, packaging, and upgrade evidence remain the intended product boundary. | Corrective PR [#2726](https://github.com/RakuenSoftware/aimee/pull/2726): restored around Go `runtime-web`, `control-web`, and config-store decisions with mechanical packaging/adapters. |
| #2692 | [`proposal-evidence-provenance-tiers.md`](pending/proposal-evidence-provenance-tiers.md) | **Misrejected** | The anti-poisoning objective is live; memory policy belongs in Go while existing C/SQL seams transport, persist, migrate, and fail closed. | Corrective PR [#2694](https://github.com/RakuenSoftware/aimee/pull/2694). |

## Remaining corrective contracts

The remaining misrejections are not unowned deferrals. Each gets one atomic PR so reviewers can
accept or reject its policy boundary without coupling it to unrelated proposal lifecycle changes.
The default order after #2662 is #2678, #2686, #2688, then #2690:

- **#2678, per-user content visibility.** Owner: the required Go `execution-policy` boundary.
  Dependencies: workspace supplies validated actor/project/resource identity, and PostgreSQL RLS is
  a mechanical enforcement adapter rather than the content-authorization owner. Lifecycle
  acceptance: restore the proposal to pending with fail-closed actor/project decisions, explicit
  migration authority, and negative cross-tenant evidence.
- **#2686, capability-scoped agent execution.** Owners: Go delegates for dispatch and the required
  Go execution-policy boundary for authorization. Dependency: one immutable effective capability
  set resolved before either disclosure or execution. Lifecycle acceptance: restore pending with
  disclosure/dispatch parity, denial evidence, and no C-owned policy decision.
- **#2688, persona-authored outputs.** Owner: Go `response-composition` for the authored envelope,
  voice, and provenance. Dependencies: Go delegates supplies persona/permission evidence, and
  execution-policy authorizes side effects. Lifecycle acceptance: restore pending with compatible
  output composition plus explicit denial and impersonation evidence.
- **#2690. Runtime/Control product boundary.** Owners: the existing Go `runtime-web` and
  `control-web` processes, with effective settings owned by the Go config store. Packaging remains
  mechanical delivery work. Lifecycle acceptance: restore pending with truthful process separation,
  omit/readiness behavior, effective configuration, transport, packaging, and upgrade evidence.

This queue is part of the audit acceptance contract. A corrective PR is complete only when its row
links the published PR and its proposal is no longer left under `rejected/`; completing #2662 does
not discharge any later row.

This audit is a lifecycle verdict, not implementation-completion evidence. A corrective PR restores
a misrejected proposal to `pending/`; the restored proposal remains unresolved until its rewritten
acceptance criteria are implemented and pass. The corrective sequence must update each tracking cell
with its PR link as soon as that draft is published.
