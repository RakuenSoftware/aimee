# ir module

## Purpose and non-goals

IR is required core and defines Aimee's provider-neutral request, response, block, tool-call, delta,
usage, stop-reason, and panel-result shapes. `aimee_request_t`, `aimee_response_t`, and
`aimee_panel_result_t` let core stages operate once
instead of repeating logic for every wire protocol. IR does not choose a provider, authorize tools,
route work, or define a provider's external JSON contract.

## Public contracts

`src/modules/ir/module.yaml` is the complete module-local ownership inventory: two production
translation units, three canonical public headers, three direct contract tests, and
`docs/modules/ir.md`.
`ownership_complete: true` makes unlisted module-local C or private-header files and omission of the
canonical document fail descriptor validation. Canonical public-header placement remains enforced by
the descriptor-derived header-layout gate. This completeness claim is about physical ownership; it does
not claim that every exported helper is a live production API.

The canonical implementations are `src/modules/ir/aimee_ir.c` and
`src/modules/ir/aimee_ir_metrics.c`. Their matching public contracts are
`src/modules/ir/include/aimee/ir/aimee_ir.h`,
`src/modules/ir/include/aimee/ir/aimee_ir_metrics.h`, and
`src/modules/ir/include/aimee/ir/panel_result.h`. Consumers use the canonical include namespace
`aimee/ir`; the former flat `src/headers` and `src/server` paths are retired.

The deferred flat `src/headers/aimee_ir_{rescue,serve,shadow,stream}.h` contracts include the canonical
IR headers for compatibility. New code should include `aimee/ir/aimee_ir.h` or
`aimee/ir/aimee_ir_metrics.h` directly when it uses those contracts rather than relying on that transitive
re-export.

These contracts own the provider-neutral message model, its allocation and accessors, IR-local metrics,
and bounded panel findings, evidence, answered questions, coverage gaps, and result status. IR does not
own panel seats, turns, convergence, deadlines, cancellation callbacks, provider selection, activation,
or result destruction; those are provider behavior rather than messages.
The optional `roundtable` provider produces this panel result today, while core serializers, evidence
replay, and workflows consume it without depending on roundtable's private execution headers.
The legacy-named rescue, serve, shadow, and stream files remain outside this ownership set because
they mix canonical operations with translation or comparison behavior. A later behavior-separation slice
must split those responsibilities before assigning them to IR or translation. IR does not own provider
ingress parsing, provider-specific body construction, response composition outside the typed result, or
`shadow_mirror.c` lifecycle behavior.

## Dependencies and consumers

- `module-runtime`: supplies the required lifecycle and extension contracts used by every core profile.

Consumers include translation, routing, gateway, protocols, memory injection, response-composition,
delegates, tools, server ingress/egress, streaming, shadow comparison, and tests. These consumers should
depend on canonical `aimee_*` types instead of reaching into another protocol's parsed JSON shape.

## Providers and readiness

IR is a deterministic in-process core library with no replaceable provider or optional service.
Readiness means canonical allocation/free, parse/serialize, block accessors, streaming deltas, and rescue
paths are linked for the selected binary. A missing inference provider may stop a turn, but it does not
make the `ir` contract unavailable or optional.

## Configuration and activation

- `runtime_toggle.supported`: `false`; IR is present in every profile and has no enable switch.

Shadowing, rescue, streaming, or parity settings tune consumers and diagnostics rather than the existence
of IR. Configuration must not select a second request representation; new wire-specific settings belong
to protocols or translation while canonical structural limits remain enforced by `aimee_ir_*` APIs.

## Surfaces

The current IR surface exposes installed C headers and symbols, canonical blocks/deltas, and the
`aimee_ir_build_provider_body` compatibility seam; provider-body conversion moves to target translation.
IR also exposes stream events, metrics, and shadow/rescue records. It owns no standalone CLI, HTTP route, listener,
dashboard, or database. External JSON and stdio/network framing are protocol surfaces translated at the
boundary into or out of `aimee_request_t` and `aimee_response_t`.

## Data and migrations

Most IR values are per-turn heap state released by `aimee_request_free` or `aimee_response_free`.
The producing provider owns `aimee_panel_result_t.artifact` and exposes the matching result-release
operation. A shallow struct copy is a non-owning view of that allocation: it neither transfers nor
duplicates ownership and must never release `artifact` independently.
Persisted transcripts, shadow comparisons, metrics, and run events are owned by their storage modules but
must retain block type/order, tool identifiers, stop reason, usage, and model semantics. Structure changes
therefore require explicit schema and wire compatibility review.

## Security and privacy

IR preserves structural separation between visible `TEXT`, hidden `THINKING`, tool calls, system content,
and usage; `aimee_ir_response_text` enforces type-strict extraction and
`src/tests/test_aimee_ir.c` asserts that thinking contributes no answer text. Consumers must not flatten
those distinctions before policy and redaction. Raw sidecars and
shadow material can contain private prompts or credentials, so diagnostics must bound and sanitize them,
and canonical data never supplies authorization merely because it parsed successfully.

## Supported journeys

An ingress adapter parses its external request into `aimee_request_t`; memory, routing, policy, and tools
then inspect or mutate typed fields; translation builds the selected provider request; returned deltas or
responses are normalized and serialized to the client protocol. Flat CLI/TUI text enters the same journey
through `aimee_ir_response_from_text` rather than a parallel answer type.

## Tests and failure behavior

The descriptor owns `src/tests/test_aimee_ir.c`, `src/tests/test_aimee_ir_metrics.c`, and
`src/tests/test_panel_ir_contract.c` as its direct contracts. The panel contract test pins bounds, enum
values, zero initialization, fixed-array copying, and artifact-pointer semantics.
Cross-protocol, ingress, backend, and parity tests exercise adjacent translation and gateway boundaries
without becoming IR-owned. Allocation or malformed-structure failure must be
explicit and leave outputs freed/zeroed; unsupported block loss must never be silent.

## Operational diagnostics

Use `aimee_ir_metrics`, stream terminal events, rescue counters, shadow comparisons, and wire-parity tests
to determine whether failure occurred during parsing, canonical mutation, provider serialization, or
egress. Logs should name the wire, stage, block type, and bounded error without dumping private raw
requests or treating every provider failure as an IR parse failure.

## Compatibility

The layout and semantics of exported `aimee_*` structures, panel result bounds and evidence enum values,
block ordering, tool-call identity, stop reasons, usage, free functions, and stream event ordering are
compatibility contracts. Future moves at the
translation or response-composition boundaries must preserve these installed headers, symbols, fixtures,
and parity baselines.

The `aimee_ir_build_provider_body` compatibility seam keeps its existing signature, ownership semantics,
and call sites until provider-body conversion moves into the target translation module. That separate
slice must preserve wire-parity baselines. The legacy rescue, serve, shadow, and stream files remain in
their current locations here; their callers, headers, and observable behavior are frozen until a
behavior-separation slice assigns each responsibility to IR or translation. Symbol removal, signature
changes, or behavior changes in those deferred files require an explicit compatibility record and consumer
migration.

## Extension and removal

Add a canonical field only when at least two consumers need the semantic concept and every relevant wire
can preserve or explicitly reject it. Per-provider convenience fields belong in translation sidecars, not
the core type. Duplicate legacy representations should be removed after caller inventories and parity
tests prove replacement. Removal of the canonical `ir` contract would break every supported execution
journey and is disallowed.

The [slice 31 liveness audit](../validation/core-modularization-slice-31.md) found both production sources
in the shipping server build with non-test consumers. It also records three narrower test-support or
currently test-only API cleanup candidates. Removing or privatizing them requires a separate
compatibility-focused slice rather than being hidden in an ownership-metadata change.
