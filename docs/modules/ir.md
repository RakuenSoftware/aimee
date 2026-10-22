# ir module

## Purpose and non-goals

IR is required core and defines Aimee's provider-neutral request, response, block, tool-call, delta,
usage, stop-reason, and panel-result shapes. `aimee_request_t`, `aimee_response_t`, and
`aimee_panel_result_t` let core stages operate once
instead of repeating logic for every wire protocol. IR does not choose a provider, authorize tools,
route work, or define a provider's external JSON contract.

## Public contracts

`src/modules/ir/module.yaml` is the complete module-local ownership inventory: production
translation units, canonical public headers, direct contract tests, and
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

### Canonical session contract (migration in progress)

`aimee/ir/session_event.h` serializes typed IR messages and deltas without native provider
sidecars. Messages carry roles and stable message identities; opaque tool IDs correlate uses
and results across message boundaries. Harness control requests/responses are distinct events,
so serializing a control never grants a model authority to approve it. Host execution lifecycle
is separate from each model response's `TURN_STOP`. Session identity, actor, sequence, and
producer retry identity belong to the transport envelope owned by Server sessions.

The `replay_proof` metadata event reserves the host's native replay evidence: a connection/account
scope digest, exact canonical block digest, source wire and (for Anthropic) the actual sent-prefix
binding. It contains no credentials or transcript. C, Go and browser consumers validate the same
shape; history and browser projection advance past it without creating model messages. Parsing
this event alone never establishes provenance or authorizes replay. The scoped native runtime captures evidence from actual upstream canonical responses and
checks it at entry and after restored history/policy edits, before egress. SDK spawn-site
activation and live cache/history adoption remain separate, unfinished work.

The host-only `native_checkpoint` event commits an attested scope, current-context digest,
canonical-prefix digest and message count (1–4096), harness (`codex` or `claude`), opaque native
handle and random claim identity. It contains no prompt or credential. C, Go and browser validate
its shape; model-history loading and browser projection advance past it without creating model
content. The native cache controller additionally verifies the owner, source-log continuity,
completed source turn and canonical history, and atomically consumes a versioned checkpoint before
handing out a resume handle. Parsing metadata alone grants no replay authority. Production SDK
callers have not selected this controller; native adoption/profile/compaction fidelity remains a
separate qualification before activation.

Thinking deltas can carry `thinking_id` and `encrypted_content` at `BLOCK_START` or
`BLOCK_DELTA`, using the same field names as complete blocks. These are complete opaque values,
not fragments: C assembly and browser replay retain late arrivals, accept identical repeats,
and refuse replacement. The fields are invalid on other kinds or lifecycle events. A target
stream renderer without an opaque-state representation must refuse the delta; any deliberate
projection and its fidelity decision belong at the translation boundary.

Unknown blocks fail serialization until an entry adapter models them; they are not silently
flattened into text. The optional thread-bound sink is scoped by its caller, which must restore
the previous sink before releasing its context or returning a pooled worker. Sink failures
propagate to the producer. IR owns no sockets, database handles, authorization, or persistence.

Browser chat uses this contract for live delivery and durable replay. Native delegate execution
retains owned IR through governance and tool dispatch; its persisted history contains canonical
`role`/`blocks` messages. Provider request histories are rendered at the egress boundary. Legacy
provider arrays cross an explicit migration adapter before execution; mixed or unknown content
fails admission rather than becoming an empty response.

Buffered responses publish `TURN_START`, a complete message, and `TURN_STOP`, retaining canonical
stop reasons and input/output, cache-read/write and reasoning usage. A `TURN_STOP` may also
carry optional `refusal` diagnostics: either null (no details), or an exact object with
`category` and `explanation`, each a string or null. Non-null details require canonical
`content_filter`; the native stop spelling remains independently preserved. Category is bounded
to 64 UTF-8 bytes and explanation to 4096; NUL, duplicate/unknown fields and malformed values
are rejected. Unknown categories are retained as inert diagnostics. These fields never become
model messages, tool instructions, permissions or automatic retry actions. Go durable admission
and browser live/replay projection retain the same contract. The browser exposes them as escaped,
read-only provider-refusal details even when the assistant has no content blocks. Chat Completions
and Responses wire projections cannot represent these structured diagnostics and explicitly refuse
non-null details, including an object whose two fields are null. The canonical response remains
unchanged; legacy `content_filter` responses without details retain their existing rendering.
No explanation is invented as model text. Provider failures publish
`ERROR` with a typed error message. Streamed harness snapshots use the existing native stream's
lifecycle and do not manufacture another turn. Canonical message decoding preserves opaque tool
IDs, signatures, encrypted/redacted reasoning, media references and context metadata. MIME type
is optional for URL media. Tool input is opaque JSON; completed API function calls additionally
require an object at their entry adapter.

The Responses backend now emits canonical text, refusal text, reasoning summaries and namespaced
function-argument deltas while the provider connection is open. The entry adapter bounds framing
and owned state, reconciles emitted prefixes and completed items against the existing full native
decoder, and refuses conflicting identities, rewritten content, invalid arguments and truncation.
`message_group` on a text `BLOCK_START` preserves authored message/part boundaries across C, Go and
browser consumers; it is immutable after opening the block.

The Responses frontend renders those deltas from canonical state. Full snapshots, policy completion
and durable history precede item-done and client-completion events. No partial stream is retried.
`/v1/responses` selects this transport for Responses-speaking providers when response governance
and subagent prevention/completion repair do not require buffered admission; other upstream wires and those policy
modes retain their existing buffered path. Incremental Responses-to-Anthropic projection remains
unfinished. The decoder's native framing stays inside translation and never becomes core history.

The migration remains incomplete: production SDK activation, full native tool effects and human
controls, account-bound cache/history admission, credential custody, cross-wire incremental
Responses ingress and remaining IR consumers still require cutover. The current
[handoff](../IR_MIGRATION_HANDOFF.md) records the implemented and unfinished boundaries.

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

The generic module-plan executor accepts host-only plans through named command
discovery. It snapshots roles, tool names and last-user text, then validates every
step before invoking any supplied connection or editing the IR. Bindings and
text resources are explicitly supplied by the host; plans cannot invoke arbitrary
commands. Supported steps call a binding, append typed context or remove a
validated descending list of catalog paths. Context metadata follows the existing
IR authority-promotion rule, revision epochs retain their full 64-bit value,
and appended blocks preserve the existing cached prefix. Domain selection,
rendering and policy belong to the Go module producing the plan.


The Go public wire contract lives in `server-go/ir`, alongside the existing
implementation-independent caller contracts. It contains typed canonical blocks,
messages, requests and catalogs plus strict codecs; it owns no storage, I/O,
module dispatch or policy. DB1 session admission consumes that shared contract.
No peer-module implementation import or bus-boundary exemption is added.

Request planning receives an explicitly marked `gateway_policy` projection of
canonical roles, catalog kinds/namespaces/provider identity and bounded last-user
text. Prompt bodies, tool schemas and opaque configuration stay with the host;
the Go policy returns descending catalog paths, and the host validates the whole
patch before changing owned IR. This keeps long transcripts out of the bounded
bus frame without flattening a native request or losing nested tool identity.


Native cache recovery only invalidates an exact abandoned claim after host proof
that its child stopped and source proof of a terminal turn; it never reconstructs
canonical history from vendor files. Finalization rejects interleaved later
source turns. Runtime cache scope construction binds captured host permissions
and source authority to separately attested SDK/launch/workspace digests, while
opaque replay provenance keeps its existing account/model identity. Production
SDK call-site activation and those concrete attestations remain unfinished.


The `native_compaction` host event has exactly one payload field, `harness`
(`codex` or `claude`). Native adapters publish it for SDK compaction boundaries;
it advances replay cursors without replacing model messages with a vendor
summary. A native cache cannot be checkpointed after this event in its source
turn. Once the child is quiescent, abort/recovery discards the derived cache and
the next execution adopts canonical history afresh. Durable capture refusal
prevents accepting an unrecorded compaction observation.
