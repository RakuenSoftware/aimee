# ir module

## Purpose and non-goals

IR is required core and defines Aimee's provider-neutral request, response, block, tool-call, delta,
usage, and stop-reason shapes. `aimee_request_t` and `aimee_response_t` let core stages operate once
instead of repeating logic for every wire protocol. IR does not choose a provider, authorize tools,
route work, or define a provider's external JSON contract.

## Public contracts

The current public contract is `src/headers/aimee_ir.h` with streaming, serving, metrics, rescue, and
shadow seams in sibling `aimee_ir_*.h` headers. Implementations currently live under `src/server`,
including `aimee_ir.c`, `aimee_ir_serve.c`, and `aimee_ir_stream.c`; the descriptor-only
`src/modules/ir` directory is the target owner, so present placement is explicit migration debt.

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

`test_aimee_ir.c`, `test_aimee_ir_stream.c`, `test_aimee_ir_serve.c`, `test_aimee_ir_rescue.c`,
`test_ir_crossproto_egress.c`, and `test_ir_legacy_parity.c` cover ownership, streaming, provider serving,
recovery, cross-protocol conversion, and parity. Allocation or malformed-structure failure must be
explicit and leave outputs freed/zeroed; unsupported block loss must never be silent.

## Operational diagnostics

Use `aimee_ir_metrics`, stream terminal events, rescue counters, shadow comparisons, and wire-parity tests
to determine whether failure occurred during parsing, canonical mutation, provider serialization, or
egress. Logs should name the wire, stage, block type, and bounded error without dumping private raw
requests or treating every provider failure as an IR parse failure.

## Compatibility

The layout and semantics of exported `aimee_*` structures, block ordering, tool-call identity, stop
reasons, usage, free functions, and stream event ordering are compatibility contracts. Moving source from
`src/server` into `src/modules/ir` must update both build graphs atomically and preserve installed headers,
symbols, fixtures, and parity baselines.

## Extension and removal

Add a canonical field only when at least two consumers need the semantic concept and every relevant wire
can preserve or explicitly reject it. Per-provider convenience fields belong in translation sidecars, not
the core type. Duplicate legacy representations should be removed after caller inventories and parity
tests prove replacement. Physical relocation into `src/modules/ir` is planned; removal of the canonical
`ir` contract would break every supported execution journey and is disallowed.
