# response-composition module

## Purpose and non-goals

Response-composition is required core: it turns a routed model/delegate result plus recalled evidence,
tool results, and policy state into the assistant response delivered to the user. It owns ordinary answer
assembly and provider-neutral response semantics; it does not own heavyweight cross-document knowledge
curation, which belongs to optional `kb-synthesis`, or roundtable-specific panel aggregation.

## Public contracts

The target contract for response-composition is one provider-neutral `aimee_response_t` plus assembly and
finalization paths spread through server, gateway, delegate, and translation code. The shape, allocation,
free, and accessor symbols for `aimee_response_t`, including `aimee_ir_response_from_text`, are owned by
the `ir` module at `src/modules/ir/include/aimee/ir/aimee_ir.h`; response-composition consumes that
canonical shape but does not own it. The descriptor is ahead of physical ownership:
`src/modules/response-composition` contains no implementation yet, a tracked relocation gap rather than
evidence that core composition is absent.

## Dependencies and consumers

- `config`: supplies response limits, provider and presentation policies used during composition.
- `ir`: supplies canonical response blocks, stop reasons, tool calls, and usage independent of wire format.
- `memory`: supplies scoped evidence and context that ground normal responses.
- `module-runtime`: supplies required lifecycle and extension contracts around the composition path.
- `skills`: supplies selected user/project instructions that shape the response journey.

Consumers are gateway/protocol serializers, agent and delegate runtimes, workflow delivery, CLI/TUI
clients, and optional modules such as roundtable that add inputs without owning the
final provider-neutral answer contract.

## Providers and readiness

Composition itself is deterministic core and is ready when IR construction/finalization and the selected
delivery serializer are available. A routed inference provider supplies content but is not a replaceable
composition implementation; provider failure can prevent an answer, while omission of `kb-synthesis`
must leave ordinary memory-backed response composition fully functional.

## Configuration and activation

- `runtime_toggle.supported`: `false`; every supported interactive or agent profile requires response composition.

Configuration tunes limits, caching placement, liveness, streaming, and delivery behavior rather than
turning composition off. The eventual canonical module must consume those keys through `config` and
remove duplicate per-ingress choices; GUI fields are valid only where the compiled response path reads
them.

## Surfaces

Surfaces include canonical IR response blocks and deltas, OpenAI/Anthropic wire responses, delegate final
messages, streamed events, CLI/TUI output, and workflow/channel delivery. Roundtable synthesis and KB
narrative endpoints are consumers or separate modules; their specialized prompts and artifacts must not
be relabeled as the universal `response-composition` surface.

## Data and migrations

Normal composition is primarily per-turn `aimee_response_t` state and owns no independent durable database schema today;
transcripts, tool results, and usage are persisted by their owning modules. Relocation must preserve block
ordering, thinking/text separation, tool-call identity, stop reason, usage, streaming boundaries, and
serialized bytes where parity baselines require them.

## Security and privacy

Composition must keep reasoning blocks distinct from user-visible `TEXT`, preserve provenance and scope
on recalled context, and never treat memory or skill text as authorization. Before delivery it must honor
execution policy, redaction, route identity, and channel constraints; diagnostics may describe structure
without leaking hidden reasoning, credentials, or private context.

## Supported journeys

For a normal request, routing selects execution, memory supplies grounded context, skills shape behavior,
the provider produces canonical deltas/blocks, and response-composition emits one coherent final answer
or tool continuation. `aimee_ir_response_from_text` also normalizes flat CLI/TUI results so downstream
consumers receive the same `aimee_response_t` contract.

## Tests and failure behavior

IR shape, OpenAI/Anthropic shape, gateway mutation/wire, streaming, liveness, and `agent-runtime` tests are
the current distributed coverage for composition. Allocation, malformed provider output, invalid tool
blocks, and empty terminal responses must fail or invoke bounded liveness behavior; hidden reasoning must
never be substituted as an answer merely to make a response non-empty.

## Operational diagnostics

Use route/provider logs, canonical stop reason and usage, streaming terminal events, liveness notices, and
wire-shape tests to locate a composition failure. Diagnostics should say whether failure occurred during
provider parsing, IR assembly, tool continuation, finalization, translation, or delivery instead of using
the ambiguous label `synthesis` for all of them.

## Compatibility

`aimee_response_t`, block order/types, stop reasons, tool IDs/arguments, usage, stream termination, and
wire serializers are compatibility contracts. Consolidation into the module may delete duplicated glue,
but output bytes governed by parity and public route/CLI baselines cannot change without an explicit
compatibility record and consumer migration.

## Extension and removal

Move distributed finalization code behind one provider-neutral contract in small slices, proving each
consumer and deleting old branches as it moves. Specialized aggregators should contribute canonical IR
instead of cloning final-answer logic. Response-composition cannot be removed from core; optional
`kb-synthesis` may enrich memory independently and must not become a required answer-path dependency.
