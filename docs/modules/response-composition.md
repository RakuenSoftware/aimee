# response-composition module

## Purpose and non-goals

Response-composition turns routed model/delegate results, recalled evidence, tool results, and policy
state into the assistant response delivered to the user. The first separately owned live stage is the
deterministic response deduplication key used by `aimee-server`. It does not own heavyweight
cross-document curation, which belongs to optional `kb-synthesis`, or roundtable-specific panel
aggregation.

### Process and bus boundary

`response-composition` is a separately shipped process attached to the server container's local event
bus. It is not a network service, does not attach to the KB bus, and is never used as transport between
machines. The daemon admits it only when its executable identity and installed event-kind grant match.

The public stage schema is
`src/modules/response-composition/include/aimee/response-composition/module_api.h`. Requests carry the
principal, source, provider, model, endpoint, explicit idempotency key, body, context, behavior flags,
and streaming mode as bounded length-prefixed fields. The module replies with the canonical key. Bodies
larger than one slot use the core wire-v3 fragment transport, with the core-owned assembled-message
limit, deadline, and cancellation behavior.

The live server registers only the event-bus-backed provider during startup. If the process is absent,
returns malformed data, is cancelled, or misses the deadline, key generation fails closed and readiness
reports `response-composition` as missing. The server does not retain a second local key implementation.

The bounded deduplication-key process implementation now runs in the shared
pure-Go module runtime. It preserves the RKEY/COMP schema, unusual historical
FNV constants, C-string principal behavior, and exact golden output. The former
C adapter remains a parity fixture. Broader response finalization is still a
relocation boundary and has not been represented as migrated by this stage.

## Public contracts

The broader provider-neutral response contract remains `aimee_response_t`. Its shape, allocation, free,
and accessor symbols, including `aimee_ir_response_from_text`, are owned by `ir` at
`src/modules/ir/include/aimee/ir/aimee_ir.h`. Response-composition consumes that canonical shape but does
not own it. Finalization code outside the deduplication stage remains a tracked relocation boundary; it
must move behind explicit stages before the old server glue can be deleted.

## Dependencies and consumers

- `config`: supplies response limits, provider, and presentation policies.
- `ir`: supplies canonical response blocks, stop reasons, tool calls, and usage.
- `memory`: supplies scoped evidence and context that ground normal responses.
- `module-runtime`: supplies admission, lifecycle, deadline, cancellation, and event-bus calls.
- `skills`: supplies selected user/project instructions that shape the response journey.

Consumers are gateway/protocol serializers, agent and delegate runtimes, workflow delivery, CLI/TUI
clients, and optional modules such as roundtable that add inputs without owning the final
provider-neutral response contract.

## Providers and readiness

The process must be attached before the server advertises readiness. A routed inference provider supplies
content but is not a replaceable composition implementation; provider failure can prevent an answer,
while omission of optional `kb-synthesis` must leave ordinary memory-backed composition functional.

## Configuration and activation

- `runtime_toggle.supported`: `false`; every supported interactive or agent profile requires response
composition. Configuration tunes limits, caching placement, liveness, streaming, and delivery behavior
rather than turning composition off. Configuration fields are valid only where a compiled response stage
actually consumes them.

## Surfaces

Surfaces include canonical `aimee_response_t` IR blocks and deltas, OpenAI/Anthropic wire responses, delegate final
messages, streamed events, CLI/TUI output, and workflow/channel delivery. Roundtable synthesis and KB
narrative endpoints are consumers or separate modules; their specialized prompts and artifacts are not
the universal response-composition surface.

## Data and migrations

Normal composition is per-turn state and owns no independent durable database schema today. Transcripts,
tool results, and usage are persisted by their owning modules. Relocation of `aimee_response_t` handling must preserve block ordering,
thinking/text separation, tool-call identity, stop reason, usage, streaming boundaries, and serialized
bytes where parity baselines require them.

## Security and privacy

Composition keeps `AIMEE_BLK_THINKING` reasoning blocks distinct from user-visible text, preserves provenance and scope on
recalled context, and never treats memory or skill text as authorization. Before delivery it must honor
execution policy, redaction, route identity, and channel constraints. Diagnostics may describe structure
without leaking hidden reasoning, credentials, or private context.

## Supported journeys

For a normal request, routing selects execution, memory supplies grounded context, skills shape behavior,
the provider produces canonical deltas/blocks, and response-composition emits one coherent final answer
or tool continuation. `aimee_ir_response_from_text` also normalizes flat CLI/TUI results so downstream
consumers receive the same `aimee_response_t` contract.

## Tests and failure behavior

The `test_response_dedup` process-handler test fixes the deduplication-key vectors and malformed-request behavior. Event-bus
runtime tests cover fragmented request/reply, cancellation, timeout, oversize draining, and capability
absence. IR shape, OpenAI/Anthropic shape, gateway mutation/wire, streaming, liveness, and agent-runtime
tests cover the broader composition path. Hidden reasoning must never be substituted as an answer merely
to make a response non-empty.

## Operational diagnostics

Use readiness `diagnostics.missing_module`, route/provider logs, canonical stop reason and usage, streaming
terminal events, liveness notices, and wire-shape tests to locate a failure. Diagnostics should identify
provider parsing, IR assembly, tool continuation, finalization, translation, or delivery instead of using
the ambiguous label `synthesis` for all of them.

## Compatibility

`aimee_response_t`, block order/types, stop reasons, tool IDs/arguments, usage, stream termination, module
wire schemas, and public serializers are compatibility contracts. A wire-incompatible core change bumps
the core/module release and repository lock together. `scripts/export_c_repositories.py` exports this
module process, grant, sources, and documentation to `aimee-module-response-composition`.

## Extension and removal

Move distributed finalization code behind provider-neutral process stages in small slices, prove each
consumer, and delete the replaced server branches as they move. Specialized aggregators contribute
canonical IR instead of cloning final-answer logic. Optional `kb-synthesis` may enrich memory independently
and must not become a required answer-path dependency.
